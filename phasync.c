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

#include "phasync_internal.h"

/* phasync.c is the only translation unit: it #includes every other subsystem file below, in this fixed order, so they
 * share one set of static definitions exactly as one un-split phasync.c
 * would. Splitting the source into files is for editing, not for separate
 * compilation -- none of them is built standalone (see config.m4), and none
 * includes another. Where one file's static function or file-scope global
 * is used by code in a file that comes before it here, that earlier file
 * still carries its own forward declaration (as the pre-split phasync.c
 * did) -- see phasync_internal.h for the ones shared by more than one. */
#include "poller.c"
#include "suspension.c"
#include "io.c"
#include "blocking.c"
#include "fs.c"
#include "pool.c"
#include "coroutine.c"
#include "sapi.c"
#include "keywords.c"
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

/* issue #42: whether a de-blocked pipe read/write took the preadv2()/pwritev2()
 * RWF_NOWAIT path, or fell back to toggling O_NONBLOCK. Process-wide, never reset. */
ZEND_FUNCTION(phasync_ext_io_stats)
{
	ZEND_PARSE_PARAMETERS_NONE();

	array_init(return_value);
	add_assoc_long(return_value, "pipe_nowait", (zend_long) PHASYNC_G(sw_pipe_nowait));
	add_assoc_long(return_value, "pipe_fcntl_fallback", (zend_long) PHASYNC_G(sw_pipe_fcntl_fallback));
}

static ZEND_INI_MH(phasync_update_fs_offload)
{
	if (zend_string_equals_literal_ci(new_value, "network")) {
		PHASYNC_G(fs_offload) = PHASYNC_FS_OFFLOAD_NETWORK;
	} else if (zend_string_equals_literal_ci(new_value, "all")) {
		PHASYNC_G(fs_offload) = PHASYNC_FS_OFFLOAD_ALL;
	} else if (zend_string_equals_literal_ci(new_value, "none")) {
		PHASYNC_G(fs_offload) = PHASYNC_FS_OFFLOAD_NONE;
	} else {
		return FAILURE;
	}
	PHASYNC_G(fs_last) = NULL;           /* decided under the old policy */
	return SUCCESS;
}

static ZEND_INI_MH(phasync_update_fs_offload_types)
{
	if (OnUpdateString(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage) != SUCCESS) {
		return FAILURE;
	}
	if (PHASYNC_G(mountinfo_fd) >= 0) {  /* reload the mount table on next use */
		close(PHASYNC_G(mountinfo_fd));
		PHASYNC_G(mountinfo_fd) = -1;
	}
	PHASYNC_G(mounts_sig) = 0;
	PHASYNC_G(fs_last) = NULL;
	return SUCCESS;
}

static ZEND_INI_MH(phasync_update_test_unknown_layouts)
{
	phasync_test_unknown_layouts = ZSTR_LEN(new_value) > 0 && ZSTR_VAL(new_value)[0] != '0';
	return SUCCESS;
}

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("phasync.thread_pool_size", "8", PHP_INI_SYSTEM, OnUpdateLong,
		thread_pool_size, zend_phasync_globals, phasync_globals)
	PHP_INI_ENTRY("phasync.fs_offload", "network", PHP_INI_ALL, phasync_update_fs_offload)
	STD_PHP_INI_ENTRY("phasync.fs_offload_types", "", PHP_INI_ALL, phasync_update_fs_offload_types,
		fs_offload_types, zend_phasync_globals, phasync_globals)
	PHP_INI_ENTRY("phasync.test_unknown_layouts", "0", PHP_INI_SYSTEM, phasync_update_test_unknown_layouts)
	PHP_INI_ENTRY("phasync.keywords", "0", PHP_INI_SYSTEM, NULL)
	STD_PHP_INI_ENTRY("phasync.gc_grace", "0", PHP_INI_SYSTEM, OnUpdateLong, gc_grace, zend_phasync_globals, phasync_globals)
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
	phasync_suspension_minit();
	pthread_atfork(phasync_preempt_fork_prepare, phasync_preempt_fork_parent, phasync_fork_child);
	zend_hash_init(&phasync_suspend_roots, 8, NULL, NULL, 1);
	zend_observer_fiber_destroy_register(phasync_ledger_fiber_destroy);
	/* The CLI's stdout hook goes under sapi()'s: output inside sapi() goes to
	 * $sapi->write() whole, not in PIPE_BUF pieces after a poll() of stdout. */
	if (strcmp(sapi_module.name, "cli") == 0) {
		phasync_ub_write_orig = sapi_module.ub_write;
		sapi_module.ub_write = phasync_ub_write;
	}
	phasync_req_minit();
#ifdef PHASYNC_HAVE_SESSION
	phasync_ps_files_startup();
#endif
	phasync_got_patch();
	phasync_vfs_register();
	phasync_conn_slot = zend_get_resource_handle("phasync");
	phasync_checkpoint_startup();
	{
		zend_class_entry *ce = register_class_phasync_Uninterruptible();

		zend_mark_internal_attribute(ce);   /* its targets are checked at compile time */
		phasync_uninterruptible_lcname = zend_string_tolower_ex(ce->name, 1);
	}
	phasync_stdio_read_orig = php_stream_stdio_ops.read;
	phasync_stdio_write_orig = php_stream_stdio_ops.write;
	phasync_stdio_set_option_orig = php_stream_stdio_ops.set_option;
	phasync_stdio_close_orig = php_stream_stdio_ops.close;
	php_stream_stdio_ops.close = phasync_stdio_close;
	php_stream_stdio_ops.read = phasync_stdio_read;
	php_stream_stdio_ops.write = phasync_stdio_write;
	php_stream_stdio_ops.set_option = phasync_stdio_set_option;
	phasync_next_minit();
	phasync_gc_minit();
	return SUCCESS;
}

static PHP_MINFO_FUNCTION(phasync)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "phasync support", "enabled");
	php_info_print_table_row(2, "version", PHP_PHASYNC_VERSION);
	php_info_print_table_row(2, "pg_* one call per connection", phasync_conn_pgsql_on ? "enabled" : "disabled (pgsql absent or its layout unrecognised)");
	php_info_print_table_row(2, "SQLite3 one call per connection", phasync_conn_sqlite3_on ? "enabled" : "disabled (sqlite3 absent or its layout unrecognised)");
	if (phasync_scope_bundled) {
		char buf[64];

		snprintf(buf, sizeof(buf), "bundled ext-scope %s", scope_module_entry.version);
		php_info_print_table_row(2, "Scope", buf);
	} else if (zend_hash_str_exists(&module_registry, "scope", sizeof("scope") - 1)) {
		php_info_print_table_row(2, "Scope", "provided by the scope extension");
	} else {
		php_info_print_table_row(2, "Scope", "provided by PHP");
	}
	php_info_print_table_row(2, "gc grace", phasync_gc_off_reason ? phasync_gc_off_reason : PHASYNC_G(gc_grace) ? "on (phasync.gc_grace)" : "off");
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
	if (phasync_scope_bundled) {
		scope_module_entry.info_func(&phasync_module_entry);
	}
}

static PHP_GINIT_FUNCTION(phasync)
{
#if defined(COMPILE_DL_PHASYNC) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	memset(phasync_globals, 0, sizeof(*phasync_globals));
	zend_hash_init(&phasync_globals->hooked, 8, NULL, phasync_hook_entry_dtor, 1);
	zend_hash_init(&phasync_globals->inflight, 8, NULL, phasync_hook_entry_dtor, 1);
	zend_hash_init(&phasync_globals->ledgers, 8, NULL, phasync_hook_entry_dtor, 1);
	zend_hash_init(&phasync_globals->wrapped_ops_cache, 8, NULL, phasync_ops_dtor, 1);
	zend_hash_init(&phasync_globals->fs_hooks, 32, NULL, NULL, 1);
	zend_hash_init(&phasync_globals->sock_hooks, 8, NULL, NULL, 1);
	zend_hash_init(&phasync_globals->conn_hooks, 64, NULL, NULL, 1);
	zend_hash_init(&phasync_globals->conn_busy, 8, NULL, NULL, 1);
	phasync_globals->mountinfo_fd = -1;
}

static PHP_GSHUTDOWN_FUNCTION(phasync)
{
	zend_hash_destroy(&phasync_globals->hooked);
	zend_hash_destroy(&phasync_globals->inflight);
	zend_hash_destroy(&phasync_globals->ledgers);
	zend_hash_destroy(&phasync_globals->wrapped_ops_cache);
	zend_hash_destroy(&phasync_globals->fs_hooks);
	zend_hash_destroy(&phasync_globals->sock_hooks);
	zend_hash_destroy(&phasync_globals->conn_hooks);
	zend_hash_destroy(&phasync_globals->conn_busy);
	for (int i = 0; i < phasync_globals->nmounts; i++) {
		free(phasync_globals->mounts[i].path);
	}
	free(phasync_globals->mounts);
	free(phasync_globals->nowait_off);
	free(phasync_globals->pipe_nowait_off);
	if (phasync_globals->mountinfo_fd >= 0) {
		close(phasync_globals->mountinfo_fd);
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
	phasync_got_restore();
	phasync_vfs_shutdown();
	phasync_pool_shutdown();
	php_stream_stdio_ops.read = phasync_stdio_read_orig;
	php_stream_stdio_ops.write = phasync_stdio_write_orig;
	php_stream_stdio_ops.set_option = phasync_stdio_set_option_orig;
	php_stream_stdio_ops.close = phasync_stdio_close_orig;
	zend_hash_destroy(&phasync_suspend_roots);
#ifdef PHASYNC_HAVE_SESSION
	phasync_ps_files_shutdown();
#endif
	zend_string_release_ex(phasync_uninterruptible_lcname, 1);
#if PHP_VERSION_ID >= 80400
	{
		zval zv;   /* the checkpoint, kept out of the function table */

		ZVAL_PTR(&zv, phasync_checkpoint_func);
		zend_function_dtor(&zv);
	}
#endif
	if (phasync_ub_write_orig) {
		sapi_module.ub_write = phasync_ub_write_orig;
	}
	phasync_next_mshutdown();
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

/* Wrap descriptor-backed streams that already exist: STDIN/STDOUT/STDERR, and
 * anything opened before the extension loaded (e.g. via ensure_loaded()'s re-exec)
 * or before the first Suspension::manage(). Sockets, FIFOs and char devices are wrapped
 * RAW. With include_files (at the first Suspension::manage(), not at RINIT), plain
 * regular files are wrapped POOL too, so a file opened before it is async in a
 * managed fiber — the same wrapping fopen() applies, and the POOL ops stay native
 * outside one. This only happens for code that uses manage(): wrapping changes the
 * stream's ops identity, which php_stream_cast(AS_STDIO) checks. Streams with no OS
 * fd (php://memory/temp, data://, userspace wrappers) or with filters are left
 * alone. */
static void phasync_wrap_existing_streams(bool include_files)
{
	zend_resource *res;
	int fd_type = php_file_le_stream();

	ZEND_HASH_FOREACH_PTR(&EG(regular_list), res) {
		php_stream *stream;
		php_socket_t fd;
		struct stat st;

		if (res == NULL || res->type != fd_type) {
			continue;
		}
		stream = (php_stream *) res->ptr;
		if (stream == NULL || stream->ops == NULL) {
			continue;
		}
		if (stream->readfilters.head || stream->writefilters.head) {
			continue;   /* filters rewrite the bytes; can't raw-read the fd */
		}
		if (stream->ops == &php_stream_userspace_ops) {
			continue;   /* a userspace wrapper's stream_cast() fd is its own to close, not ours to own */
		}
		fd = phasync_stream_fd(stream);
		if (fd == -1 || fstat(fd, &st) != 0) {
			continue;
		}
		if (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode)) {
			phasync_wrap_stream(stream, PHASYNC_MODE_RAW);   /* idempotent */
		} else if (include_files && S_ISREG(st.st_mode) && stream->ops == &php_stream_stdio_ops
		        && phasync_fs_fd_offloaded(fd)) {
			phasync_wrap_stream(stream, PHASYNC_MODE_POOL);
		}
	} ZEND_HASH_FOREACH_END();
}

/* ---- request startup and shutdown ----------------------------------------- */

/* memcached.sess_persistent shares one libmemcached handle across every
 * coroutine without the per-handle lock (that lock lives on the Memcached
 * object's methods, which the session handler bypasses) -- not fixable from
 * here (see README, Memcached). Checked at the first
 * Suspension::manage() of a request. */
static bool phasync_memcached_sessions_unsafe(void)
{
	const char *save_handler = zend_ini_string("session.save_handler", sizeof("session.save_handler") - 1, false);

	return save_handler && strcmp(save_handler, "memcached") == 0
	    && zend_ini_long("memcached.sess_persistent", sizeof("memcached.sess_persistent") - 1, false);
}

static PHP_RINIT_FUNCTION(phasync)
{
	if (phasync_scope_bundled) {
		scope_module_entry.request_startup_func(type, module_number);
	}

	phasync_suspension_rinit();
	PHASYNC_G(spawn) = NULL;
	PHASYNC_G(ub_writing) = false;
	PHASYNC_G(hooks_installed) = 0;
	zend_hash_clean(&PHASYNC_G(hooked));
	zend_hash_clean(&PHASYNC_G(inflight));
	PHASYNC_G(holds) = 0;
	/* Always-on: the transport factories and function overrides go in at the
	 * start of every request (they are inert outside a managed fiber),
	 * and streams that predate them get wrapped now. */
	phasync_install_hooks();
	phasync_req_rinit();
	phasync_gc_rinit();
	phasync_wrap_existing_streams(false);
#ifdef PHASYNC_HAVE_SESSION
	if (phasync_ps) {
		phasync_ps_files_wrap();
	}
#endif
	/* Per request, so opcache never substitutes it (see coroutine.c, preemption). */
	zend_register_bool_constant(ZEND_STRL("phasync\\ext\\__PREEMPT_DUE"), false, 0, module_number);
	PHASYNC_G(preempt_due) = &((zend_constant *) zend_hash_find_ptr(EG(zend_constants), phasync_preempt_due_name))->value;
	return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(phasync)
{
	if (PHASYNC_G(preempt)) {
		phasync_preempt_stop(PHASYNC_G(preempt));
		PHASYNC_G(preempt) = NULL;
	}
	phasync_req_rshutdown();
	phasync_restore_hooks();
	/* Leave the hooked table intact: streams may close later in shutdown. */
	/* Fibers PHP freed without resuming them (after a fatal error): settle what
	 * the extension still holds for them. */
	{
		phasync_ledger *l;
		ZEND_HASH_FOREACH_PTR(&PHASYNC_G(ledgers), l) {
			phasync_ledger_settle(l);
		} ZEND_HASH_FOREACH_END();
		zend_hash_clean(&PHASYNC_G(ledgers));
	}
	phasync_suspension_rshutdown();
	phasync_gc_rshutdown();
	if (phasync_scope_bundled) {
		scope_module_entry.request_shutdown_func(type, module_number);
	}
	return SUCCESS;
}

/* Every object is gone now. A poller still here was held by a fiber PHP freed
 * without unwinding it (after a fatal error): close it before the request's
 * memory goes. */
static ZEND_MODULE_POST_ZEND_DEACTIVATE_D(phasync)
{
	while (phasync_pollers) {
		phasync_poller_free(phasync_pollers);
	}
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
	PHP_MODULE_GLOBALS(phasync),
	PHP_GINIT(phasync),
	PHP_GSHUTDOWN(phasync),
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(phasync),
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_PHASYNC
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(phasync)
#endif
