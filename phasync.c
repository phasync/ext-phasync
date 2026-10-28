/* phasync extension
 *
 * phasync\ext\Suspension (suspension.c) — how a blocking call in a fiber reaches
 *         an event loop. A loop opts its fibers in with Suspension::manage(). The
 *         tcp/unix/ssl transports are re-registered, and the process, sleep, DNS,
 *         file and filesystem functions, stream_select()/socket_select() and
 *         friends overridden at request start; every descriptor-backed stream is
 *         wrapped as it is created (plus STDIN/STDOUT/STDERR and any fds inherited
 *         before load). The wrappers are inert outside a managed fiber: a wrapped
 *         stream then behaves exactly like an unwrapped one. In one, a would-block
 *         suspends the fiber and throws a Suspension out of the resume() that ran
 *         it; Suspension::poll() — the loop's one blocking call — finds it ready
 *         when it may continue: descriptors through one-shot epoll registrations,
 *         thread-pool tasks through a queue and an eventfd, timeouts by deadline.
 *         Resumed, the op finishes the way native PHP does, its native timeout
 *         included. Regular files, DNS and the FIFO open() rendezvous run on
 *         worker threads (not readiness-pollable).
 */

#ifdef __APPLE__
# define _DARWIN_UNLIMITED_SELECT 1   /* select() past FD_SETSIZE (the poller's select mode) */
#endif
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "phasync_php.h"
#include "phasync_arginfo.h"
#include "ext/filter/php_filter.h"
#include "main/php_variables.h"

/* phasync.c is one translation unit with the files it #includes below; suspension/
 * is another (see suspension/suspension.h), and scope/scope.c a third. */
#include "sapi.c"
#include "keywords.c"
#include "statcache.c"
#include "memo.c"
#include "gc.c"

/* Swerve's script handler (Swerve::accept()) requires an app's entry file
 * (e.g. WordPress's index.php) from inside a method, so its top-level
 * variables land as that method's locals instead of globals -- the file
 * loses its own global scope. include_global() compiles $file and runs its
 * op_array the way Zend/zend.c's zend_execute_script() runs the primary
 * script: a top-level call frame bound to EG(symbol_table), never the
 * caller's locals, no matter how deeply nested the call (a function, a
 * closure, a Fiber). Unlike zend_execute_script(), an uncaught exception
 * is left for the normal internal-function return path to propagate to our
 * own caller instead of being turned into a fatal error -- there is a PHP
 * caller here to give it to. */
ZEND_FUNCTION(phasync_ext_include_global)
{
	zend_string *filename;
	zend_op_array *op_array;
	zend_execute_data *call;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_PATH_STR(filename)
	ZEND_PARSE_PARAMETERS_END();

	op_array = compile_filename(ZEND_REQUIRE, filename);
	if (!op_array) {
		/* Already reported the way a bareword require() reports it: a
		 * missing file as E_COMPILE_ERROR, a syntax error as a thrown
		 * ParseError (EG(exception) is set in that case). */
		return;
	}

	/* A frame bound to the global symbol table owns its variables there (IS_INDIRECT to its CVs).
	 * Binding a second one with a variable of the same name would copy the value into both,
	 * with nothing to bind the first back afterwards, as require from the top level does. */
	for (uint32_t i = 0; i < op_array->last_var; i++) {
		zval *zv = zend_hash_find_known_hash(&EG(symbol_table), op_array->vars[i]);
		if (zv && Z_TYPE_P(zv) == IS_INDIRECT) {
			zend_throw_error(NULL, "phasync\\ext\\include_global(): $%s is a top-level variable of code still running in the global scope, which %s cannot share",
				ZSTR_VAL(op_array->vars[i]), ZSTR_VAL(filename));
			zend_destroy_static_vars(op_array);
			destroy_op_array(op_array);
			efree_size(op_array, sizeof(zend_op_array));
			return;
		}
	}

	call = zend_vm_stack_push_call_frame(ZEND_CALL_TOP_CODE | ZEND_CALL_HAS_SYMBOL_TABLE,
		(zend_function *) op_array, 0, NULL);
	call->symbol_table = &EG(symbol_table);
	zend_init_code_execute_data(call, op_array, return_value);
	ZEND_OBSERVER_FCALL_BEGIN(call);
	zend_execute_ex(call);
	zend_vm_stack_free_call_frame(call);

	zend_destroy_static_vars(op_array);
	destroy_op_array(op_array);
	efree_size(op_array, sizeof(zend_op_array));
}

PHP_INI_BEGIN()
	PHP_INI_ENTRY("phasync.keywords", "0", PHP_INI_SYSTEM, NULL)
	PHP_INI_ENTRY("phasync.memo_memory", "16M", PHP_INI_SYSTEM, NULL) /* memo.c; 0: off */
	PHP_INI_ENTRY("phasync.memo_min_ns", "500", PHP_INI_SYSTEM, NULL)   /* memo.c: a json_decode()/unserialize() faster than this is not stored */
	PHP_INI_ENTRY("phasync.stat_cache", "0", PHP_INI_SYSTEM, NULL)    /* statcache.c */
	PHP_INI_ENTRY("phasync.stat_cache_watches", "4096", PHP_INI_SYSTEM, NULL)
	PHP_INI_ENTRY("phasync.test_stat_cache_untrusted", "0", PHP_INI_SYSTEM, NULL)   /* tests: no filesystem is trusted */
	PHP_INI_ENTRY("phasync.gc_grace", "0", PHP_INI_SYSTEM, NULL)        /* gc.c: ms; 0: off */
PHP_INI_END()

/* Scope: bundled fiber-interop/ext-scope (scope/scope.c, a separate translation
 * unit, compiled in via config.m4). phasync drives its module lifecycle itself
 * instead of registering a second module_registry entry (unsafe while
 * zend_startup_modules() is iterating it): if a standalone "scope" extension (or,
 * in a future PHP, a core Scope class) is already present, phasync yields to it
 * and does nothing; otherwise phasync starts the bundled copy under its own
 * module_number. ZEND_MOD_OPTIONAL("scope") in phasync's deps (see
 * phasync_module_entry below) makes a standalone scope MINIT first regardless of
 * php.ini order. */
extern zend_module_entry scope_module_entry;
extern zend_module_entry phasync_module_entry;
static bool phasync_scope_bundled;

static bool phasync_scope_elsewhere(void)
{
	return zend_hash_str_exists(&module_registry, "scope", sizeof("scope") - 1)
		|| zend_hash_str_exists(CG(class_table), "scope", sizeof("scope") - 1);
}

static PHP_MINIT_FUNCTION(phasync)
{
	/* dl() or FPM's php_admin_value[extension]: after startup, too late for the
	 * observers, the zend_extension and the hooks MINIT installs (issue #9) */
	if (php_get_module_initialized()) {
		EG(current_module)->module_started = 0;   /* no MSHUTDOWN for what never started */
		zend_error(E_CORE_ERROR, "phasync cannot be loaded after startup (dl() or php_admin_value[extension]); "
			"load it with extension=phasync.so in php.ini, or with -d extension= on the php/php-fpm command line");
		return FAILURE;
	}
	REGISTER_INI_ENTRIES();
	phasync_memo_startup();
	phasync_sc_startup();
	if (INI_BOOL("phasync.keywords")) {
		phasync_keywords_minit();
	}
	if (phasync_scope_elsewhere()) {
		phasync_scope_bundled = false;
	} else {
		if (scope_module_entry.globals_size) {
#ifdef ZTS
			ts_allocate_id(scope_module_entry.globals_id_ptr, scope_module_entry.globals_size,
				(ts_allocate_ctor) scope_module_entry.globals_ctor, (ts_allocate_dtor) scope_module_entry.globals_dtor);
#else
			scope_module_entry.globals_ctor(scope_module_entry.globals_ptr);
#endif
		}
		if (scope_module_entry.module_startup_func(type, module_number) == FAILURE) {
			return FAILURE;
		}
		phasync_scope_bundled = true;
	}
	if (phasync_susp_module_startup(type, module_number) == FAILURE) {
		return FAILURE;
	}
	phasync_req_minit();
	phasync_next_minit();
	phasync_gc_minit();
	return SUCCESS;
}

static PHP_MINFO_FUNCTION(phasync)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "phasync support", "enabled");
	php_info_print_table_row(2, "version", PHP_PHASYNC_VERSION);
	phasync_susp_info();
	if (phasync_scope_bundled) {
		char buf[64];

		snprintf(buf, sizeof(buf), "bundled ext-scope %s", scope_module_entry.version);
		php_info_print_table_row(2, "Scope", buf);
	} else if (zend_hash_str_exists(&module_registry, "scope", sizeof("scope") - 1)) {
		php_info_print_table_row(2, "Scope", "provided by the scope extension");
	} else {
		php_info_print_table_row(2, "Scope", "provided by PHP");
	}
	php_info_print_table_row(2, "gc grace", phasync_gc_off_reason ? phasync_gc_off_reason : PHASYNC_GC(gc_grace) ? "on (phasync.gc_grace)" : "off");
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
	if (phasync_scope_bundled) {
		scope_module_entry.info_func(&phasync_module_entry);
	}
}

static PHP_MSHUTDOWN_FUNCTION(phasync)
{
	phasync_keywords_mshutdown();
	if (phasync_scope_bundled) {
		scope_module_entry.module_shutdown_func(type, module_number);
		if (scope_module_entry.globals_size) {
#ifdef ZTS
			if (*scope_module_entry.globals_id_ptr) {
				ts_free_id(*scope_module_entry.globals_id_ptr);
			}
#else
			if (scope_module_entry.globals_dtor) {
				scope_module_entry.globals_dtor(scope_module_entry.globals_ptr);
			}
#endif
		}
	}
	phasync_susp_module_shutdown();
	phasync_next_mshutdown();
	phasync_memo_shutdown();
	phasync_sc_shutdown();
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

/* ---- request startup and shutdown ----------------------------------------- */

static PHP_RINIT_FUNCTION(phasync)
{
	if (phasync_scope_bundled) {
		scope_module_entry.request_startup_func(type, module_number);
	}

	phasync_susp_request_startup(module_number);
	phasync_req_rinit();
	phasync_gc_rinit();
	return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(phasync)
{
	phasync_req_rshutdown();
	phasync_susp_request_shutdown();
	phasync_gc_rshutdown();
	if (phasync_scope_bundled) {
		scope_module_entry.request_shutdown_func(type, module_number);
	}
	return SUCCESS;
}

static ZEND_MODULE_POST_ZEND_DEACTIVATE_D(phasync)
{
	phasync_susp_post_deactivate();
	phasync_memo_request_end();
	return SUCCESS;
}

static const zend_module_dep phasync_deps[] = {
	ZEND_MOD_OPTIONAL("scope")
	ZEND_MOD_END
};

zend_module_entry phasync_module_entry = {
	STANDARD_MODULE_HEADER_EX, NULL, phasync_deps,
	"phasync",
	ext_functions,
	PHP_MINIT(phasync),
	PHP_MSHUTDOWN(phasync),
	PHP_RINIT(phasync),
	PHP_RSHUTDOWN(phasync),
	PHP_MINFO(phasync),
	PHP_PHASYNC_VERSION,
	NO_MODULE_GLOBALS,
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(phasync),
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_PHASYNC
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(phasync)
#endif
