/* suspension/'s translation unit: it #includes its subsystem files in this
 * fixed order, so they share one set of static definitions; the rest of the
 * extension reaches them only through suspension.h. */
#include "internal.h"

#include "poller.c"
#include "suspension.c"
#include "io.c"
#include "blocking.c"
#include "fs.c"
#include "pool.c"
#include "coroutine.c"

bool phasync_sc_own;

zend_function **phasync_header_cb_slot(void)
{
	return &PHASYNC_G(header_cb);
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

static ZEND_INI_MH(phasync_update_allow_persistent)
{
	phasync_allow_persistent = ZSTR_LEN(new_value) > 0 && ZSTR_VAL(new_value)[0] != '0';
	return SUCCESS;
}

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("phasync.thread_pool_size", "8", PHP_INI_SYSTEM, OnUpdateLong,
		thread_pool_size, zend_phasync_globals, phasync_globals)
	PHP_INI_ENTRY("phasync.fs_offload", "network", PHP_INI_ALL, phasync_update_fs_offload)
	STD_PHP_INI_ENTRY("phasync.fs_offload_types", "", PHP_INI_ALL, phasync_update_fs_offload_types,
		fs_offload_types, zend_phasync_globals, phasync_globals)
	PHP_INI_ENTRY("phasync.test_unknown_layouts", "0", PHP_INI_SYSTEM, phasync_update_test_unknown_layouts)
	PHP_INI_ENTRY("phasync.allow_persistent", "0", PHP_INI_SYSTEM, phasync_update_allow_persistent)   /* blocking.c */
PHP_INI_END()

static void phasync_globals_ctor(zend_phasync_globals *phasync_globals)
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

static void phasync_globals_dtor(zend_phasync_globals *phasync_globals)
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

zend_result phasync_susp_module_startup(int type, int module_number)
{
#ifdef ZTS
	ts_allocate_id(&phasync_globals_id, sizeof(zend_phasync_globals),
		(ts_allocate_ctor) phasync_globals_ctor, (ts_allocate_dtor) phasync_globals_dtor);
#else
	phasync_globals_ctor(&phasync_globals);
#endif
	REGISTER_INI_ENTRIES();
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
	return SUCCESS;
}

void phasync_susp_module_shutdown(void)
{
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
#ifdef ZTS
	ts_free_id(phasync_globals_id);
#else
	phasync_globals_dtor(&phasync_globals);
#endif
}

void phasync_susp_request_startup(int module_number)
{
	phasync_suspension_rinit();
	PHASYNC_G(spawn) = NULL;
	PHASYNC_G(ub_writing) = false;
	PHASYNC_G(hooks_installed) = 0;
	zend_hash_clean(&PHASYNC_G(hooked));
	zend_hash_clean(&PHASYNC_G(inflight));
	zend_hash_clean(&PHASYNC_G(conn_busy));   /* left by a bailout */
	PHASYNC_G(holds) = 0;
	/* Always-on: the transport factories and function overrides go in at the
	 * start of every request (they are inert outside a managed fiber),
	 * and streams that predate them get wrapped now. */
	phasync_install_hooks();
	phasync_wrap_existing_streams(false);
#ifdef PHASYNC_HAVE_SESSION
	if (phasync_ps) {
		phasync_ps_files_wrap();
	}
#endif
	/* Per request, so opcache never substitutes it (see coroutine.c, preemption). */
	zend_register_bool_constant(ZEND_STRL("phasync\\ext\\__PREEMPT_DUE"), false, 0, module_number);
	PHASYNC_G(preempt_due) = &((zend_constant *) zend_hash_find_ptr(EG(zend_constants), phasync_preempt_due_name))->value;
}

void phasync_susp_request_shutdown(void)
{
	if (PHASYNC_G(preempt)) {
		phasync_preempt_stop(PHASYNC_G(preempt));
		PHASYNC_G(preempt) = NULL;
	}
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
}

/* Every object is gone now. A poller still here was held by a fiber PHP freed
 * without unwinding it (after a fatal error): close it before the request's
 * memory goes. */
void phasync_susp_post_deactivate(void)
{
	while (phasync_pollers) {
		phasync_poller_free(phasync_pollers);
	}
}

void phasync_susp_info(void)
{
	php_info_print_table_row(2, "pg_* one call per connection", phasync_conn_pgsql_on ? "enabled" : "disabled (pgsql absent or its layout unrecognised)");
	php_info_print_table_row(2, "SQLite3 one call per connection", phasync_conn_sqlite3_on ? "enabled" : "disabled (sqlite3 absent or its layout unrecognised)");
}
