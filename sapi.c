/* phasync\ext\sapi(): gives each coroutine its own request.
 *
 * The SAPI-facing side of a web server written in PHP: while a coroutine runs
 * inside sapi(), PHP's output layer (output buffers, headers, the response
 * status), its request (method, query string, body behind php://input,
 * uploaded files) and the session are the coroutine's own, swapped in and out
 * when fibers switch. Fibers started inside belong to the request of the
 * fiber that starts them. What would go to the client goes to the $sapi
 * object, as a SAPI would receive it. Nothing else is isolated: not statics,
 * declarations, other globals, shutdown functions or error handlers. exit() is
 * not intercepted.
 *
 * A request starts as PHP's own do (phasync_req_startup()): its body is read for
 * a POST form, and $_GET, $_POST, $_COOKIE, $_FILES, $_SERVER and $_REQUEST are
 * registered by PHP's request-startup code, input filter, max_input_vars,
 * post_max_size and rfc1867 included. They go to the current Scope's
 * superglobals (ext-scope keeps them per Scope; the host enters the request's
 * Scope first); sapi() itself knows nothing of Scope. ext/filter's raw input
 * arrays (filter_input()) are the request's, swapped like the rest.
 *
 * ext/session's per-request state (id, status, save handler, ...) is reached
 * through phasync_ps (blocking.c's pointer to its module globals), the same
 * way the files save handler's flock hook already does, not the PS() macro
 * (it needs ps_globals_id, which ext/session does not export): the module
 * may be absent, and this stays quiet stock behaviour then. A request's
 * session is written and closed the way RSHUTDOWN would, when it returns or
 * throws. $_SESSION is the one superglobal sapi() does touch: ext/session's
 * own "_SESSION" entry in EG(symbol_table) is not an indirect alias of
 * http_session_vars, but its own zval sharing the same reference -- kept in
 * sync on every install() the same way session_start() itself keeps the two
 * together. */

#if PHP_VERSION_ID >= 80600
# define PHASYNC_REQ_HEADER_CB(X) X(send_header_fcc, SG(send_header_fcc))
#else
# define PHASYNC_REQ_HEADER_CB(X) X(callback_func, SG(callback_func)) X(fci_cache, SG(fci_cache))
#endif
#if PHP_VERSION_ID >= 80400
# define PHASYNC_REQ_PARSE_BODY(X) X(parse_body, SG(request_parse_body_context))
#else
# define PHASYNC_REQ_PARSE_BODY(X)
#endif
#ifdef PHASYNC_HAVE_SESSION
# if PHP_VERSION_ID >= 80600
#  define PHASYNC_REQ_SESS_OBJMETHODS(X) X(sess_user_obj_methods, phasync_ps->mod_user_uses_object_methods_as_handlers)
# else
#  define PHASYNC_REQ_SESS_OBJMETHODS(X)
# endif
/* ext/session's per-request state (what its RINIT/RSHUTDOWN reset). Swapped
 * separately from PHASYNC_REQ_STATE, guarded by phasync_ps != NULL (session
 * absent: skipped, stock behaviour). Its ini-backed settings (session.name,
 * the cookie parameters, ...) stay shared: the INI system owns those. */
# define PHASYNC_REQ_SESSION_FIELDS(X) \
	X(sess_id,               phasync_ps->id) \
	X(sess_status,           phasync_ps->session_status) \
	X(sess_mod,              phasync_ps->mod) \
	X(sess_mod_data,         phasync_ps->mod_data) \
	X(sess_user_names,        phasync_ps->mod_user_names) \
	X(sess_user_implemented,  phasync_ps->mod_user_implemented) \
	X(sess_user_is_open,      phasync_ps->mod_user_is_open) \
	X(sess_user_class_name,   phasync_ps->mod_user_class_name) \
	PHASYNC_REQ_SESS_OBJMETHODS(X) \
	X(sess_vars,              phasync_ps->session_vars) \
	X(sess_http_vars,         phasync_ps->http_session_vars) \
	X(sess_send_cookie,       phasync_ps->send_cookie) \
	X(sess_define_sid,        phasync_ps->define_sid) \
	X(sess_in_save_handler,   phasync_ps->in_save_handler) \
	X(sess_set_handler,       phasync_ps->set_handler) \
	X(sess_started_file,      phasync_ps->session_started_filename) \
	X(sess_started_line,      phasync_ps->session_started_lineno)
#endif

/* The live globals a request owns. Each entry is (field, the live global it mirrors). */
#define PHASYNC_REQ_STATE(X) \
	X(ob_handlers,        OG(handlers)) \
	X(ob_active,          OG(active)) \
	X(ob_running,         OG(running)) \
	X(ob_start_file,      OG(output_start_filename)) \
	X(ob_start_line,      OG(output_start_lineno)) \
	X(ob_flags,           OG(flags)) \
	X(headers,            SG(sapi_headers)) \
	X(headers_sent,       SG(headers_sent)) \
	PHASYNC_REQ_HEADER_CB(X) \
	X(header_cb,          PHASYNC_G(header_cb)) \
	X(read_post_bytes,    SG(read_post_bytes)) \
	X(post_read,          SG(post_read)) \
	X(uploaded_files,     SG(rfc1867_uploaded_files)) \
	X(request_body,       SG(request_info).request_body) \
	X(request_method,     SG(request_info).request_method) \
	X(content_type,       SG(request_info).content_type) \
	X(content_type_dup,   SG(request_info).content_type_dup) \
	X(content_length,     SG(request_info).content_length) \
	X(headers_only,       SG(request_info).headers_only) \
	X(no_headers,         SG(request_info).no_headers) \
	X(post_entry,         SG(request_info).post_entry) \
	X(proto_num,          SG(request_info).proto_num) \
	X(query_string,       SG(request_info).query_string) \
	X(request_uri,        SG(request_info).request_uri) \
	X(auth_user,          SG(request_info).auth_user) \
	X(auth_password,      SG(request_info).auth_password) \
	X(auth_digest,        SG(request_info).auth_digest) \
	PHASYNC_REQ_PARSE_BODY(X) \
	X(cookie_data,        SG(request_info).cookie_data) \
	X(argc,               SG(request_info).argc) \
	X(argv,               SG(request_info).argv) \
	X(request_time,       SG(global_request_time)) \
	X(connection_status,  PG(connection_status)) \
	X(ignore_user_abort,  PG(ignore_user_abort))

/* ext/filter's raw input (filter_input(), filter_has_var()): the request's own.
 * Swapped when ext/filter is loaded (phasync_if). */
#define PHASYNC_REQ_FILTER_FIELDS(X) \
	X(if_get,             phasync_if->get_array) \
	X(if_post,            phasync_if->post_array) \
	X(if_cookie,          phasync_if->cookie_array) \
	X(if_server,          phasync_if->server_array)

static zend_filter_globals *phasync_if;

typedef struct phasync_req_state {
#define PHASYNC_REQ_DECL(name, live) __typeof__(live) name;
	PHASYNC_REQ_STATE(PHASYNC_REQ_DECL)
	PHASYNC_REQ_FILTER_FIELDS(PHASYNC_REQ_DECL)
#undef PHASYNC_REQ_DECL
#ifdef PHASYNC_HAVE_SESSION
#define PHASYNC_REQ_SESSION_DECL(name, live) __typeof__(live) name;
	PHASYNC_REQ_SESSION_FIELDS(PHASYNC_REQ_SESSION_DECL)
#undef PHASYNC_REQ_SESSION_DECL
#endif
} phasync_req_state;

typedef struct phasync_request {
	uint32_t refcount;                 /* the sapi() call + each fiber of the request */
	bool ended;                        /* output of fibers still running is discarded */
	bool aborted;                      /* a write() failed: the bailout at that write is ours to swallow, not a real fatal/exit() */
	zend_string *name;                 /* PHP_SAPI and php_sapi_name() while installed */
	zval sapi;
	zend_function *m_write, *m_read, *m_send_headers, *m_flush, *m_connection_aborted;
	phasync_req_state st;              /* the live globals while the request is not installed */
	zval server;                       /* request_info()'s 'server': $_SERVER's own entries */
	php_stream *prebody;               /* a POST body read before PHP parses it (phasync_req_startup()) */
	bool prebody_all;                  /* ...all of it */
	php_stream *body_seen;             /* the body stream read_post() last filled... */
	HashTable *bodies_replaced;        /* ...and earlier ones PHP replaced */
} phasync_request;

#define PHASYNC_REQ_KEY(ctx) ((zend_ulong) (uintptr_t) (ctx))

static phasync_request *phasync_req_cur;           /* installed; NULL: the process's own */
static phasync_req_state phasync_req_root;
static HashTable phasync_req_fibers;               /* fiber context -> its request */
static uint32_t phasync_req_count;                 /* requests alive */

static char *phasync_req_cli_name;
static zval phasync_req_cli_val;
static int phasync_req_cli_as_text;
static zend_constant *phasync_req_sapi_const;

static size_t (*phasync_req_ub_write_prev)(const char *str, size_t len);
static int (*phasync_req_send_headers_prev)(sapi_headers_struct *h);
static void (*phasync_req_flush_prev)(void *server_context);
static size_t (*phasync_req_read_post_prev)(char *buf, size_t len);
static int (*phasync_req_header_handler_prev)(sapi_header_struct *h, sapi_header_op_enum op, sapi_headers_struct *hs);
static void (*phasync_req_orig_connection_aborted)(INTERNAL_FUNCTION_PARAMETERS);
static void (*phasync_req_orig_connection_status)(INTERNAL_FUNCTION_PARAMETERS);

static zend_always_inline phasync_request *phasync_req_of(zend_fiber_context *ctx)
{
	return zend_hash_index_find_ptr(&phasync_req_fibers, PHASYNC_REQ_KEY(ctx));
}

/* ---- installing a request: its globals swapped in for the process's or another's ---- */

static void phasync_req_install(phasync_request *in)
{
	phasync_request *out = phasync_req_cur;
	phasync_req_state *st;

	if (in == out) {
		return;
	}
	phasync_req_cur = in;

	st = out ? &out->st : &phasync_req_root;
#define PHASYNC_REQ_SAVE(name, live) st->name = live;
	PHASYNC_REQ_STATE(PHASYNC_REQ_SAVE)
#undef PHASYNC_REQ_SAVE
	st = in ? &in->st : &phasync_req_root;
#define PHASYNC_REQ_LOAD(name, live) live = st->name;
	PHASYNC_REQ_STATE(PHASYNC_REQ_LOAD)
#undef PHASYNC_REQ_LOAD
	if (phasync_if) {
		st = out ? &out->st : &phasync_req_root;
#define PHASYNC_REQ_SAVE(name, live) st->name = live;
		PHASYNC_REQ_FILTER_FIELDS(PHASYNC_REQ_SAVE)
#undef PHASYNC_REQ_SAVE
		st = in ? &in->st : &phasync_req_root;
#define PHASYNC_REQ_LOAD(name, live) live = st->name;
		PHASYNC_REQ_FILTER_FIELDS(PHASYNC_REQ_LOAD)
#undef PHASYNC_REQ_LOAD
	}

#ifdef PHASYNC_HAVE_SESSION
	if (phasync_ps) {
		st = out ? &out->st : &phasync_req_root;
#define PHASYNC_REQ_SESSION_SAVE(name, live) st->name = live;
		PHASYNC_REQ_SESSION_FIELDS(PHASYNC_REQ_SESSION_SAVE)
#undef PHASYNC_REQ_SESSION_SAVE
		st = in ? &in->st : &phasync_req_root;
#define PHASYNC_REQ_SESSION_LOAD(name, live) live = st->name;
		PHASYNC_REQ_SESSION_FIELDS(PHASYNC_REQ_SESSION_LOAD)
#undef PHASYNC_REQ_SESSION_LOAD

		/* $_SESSION: php_session_track_init() wires it as its own
		 * reference-sharing zval in EG(symbol_table), not an indirect
		 * alias of http_session_vars -- kept in sync here, the same way
		 * ext/session keeps the two together when a session starts. If
		 * nothing ever called session_start(), there is no "_SESSION"
		 * entry yet, nothing to sync, and the next one to start a
		 * session creates it (session.c's own doing). */
		{
			zval *sym = zend_hash_str_find(&EG(symbol_table), "_SESSION", sizeof("_SESSION") - 1);

			if (sym) {
				zval old;

				if (Z_TYPE_P(sym) == IS_INDIRECT) {
					sym = Z_INDIRECT_P(sym);
				}
				old = *sym;
				ZVAL_COPY(sym, &phasync_ps->http_session_vars);
				zval_ptr_dtor(&old);
			}
		}
	}
#endif

	/* issue #57: php_sapi_name() and PHP_SAPI name the request's SAPI inside one */
	if (in) {
		if (!out) {
			phasync_req_cli_name = sapi_module.name;
			phasync_req_cli_as_text = sapi_module.phpinfo_as_text;
			sapi_module.phpinfo_as_text = 0;
			ZVAL_COPY_VALUE(&phasync_req_cli_val, &phasync_req_sapi_const->value);
		}
		sapi_module.name = ZSTR_VAL(in->name);
		ZVAL_STR(&phasync_req_sapi_const->value, in->name);
	} else {
		sapi_module.name = phasync_req_cli_name;
		sapi_module.phpinfo_as_text = phasync_req_cli_as_text;
		ZVAL_COPY_VALUE(&phasync_req_sapi_const->value, &phasync_req_cli_val);
	}
}

static void phasync_req_state_init(phasync_req_state *st)
{
	memset(st, 0, sizeof(*st));
	zend_stack_init(&st->ob_handlers, sizeof(php_output_handler *));
	st->ob_flags = PHP_OUTPUT_ACTIVATED;
	zend_llist_init(&st->headers.headers, sizeof(sapi_header_struct), (void (*)(void *)) sapi_free_header, 0);
	st->headers.http_response_code = 200;
	st->headers.send_default_content_type = 1;
	if (PG(expose_php)) {
		sapi_header_struct h;

		h.header_len = sizeof(SAPI_PHP_VERSION_HEADER) - 1;
		h.header = estrndup(SAPI_PHP_VERSION_HEADER, h.header_len);
		zend_llist_add_element(&st->headers.headers, &h);
	}
#if PHP_VERSION_ID >= 80600
	st->send_header_fcc = empty_fcall_info_cache;
#else
	ZVAL_UNDEF(&st->callback_func);
	st->fci_cache = empty_fcall_info_cache;
#endif
	st->proto_num = 1000;
	st->ignore_user_abort = PG(ignore_user_abort);   /* the ini setting */
	ZVAL_UNDEF(&st->if_get);                         /* as ext/filter's input_filter_init() */
	ZVAL_UNDEF(&st->if_post);
	ZVAL_UNDEF(&st->if_cookie);
	ZVAL_UNDEF(&st->if_server);
#ifdef PHASYNC_HAVE_SESSION
	/* A fresh request's session state, as ext/session's RINIT leaves it: no
	 * session, the worker's save handler (session.save_handler, and
	 * session_set_save_handler() outside any sapi() call, as phasync_ps
	 * already has it). */
	if (phasync_ps) {
		zval *src = (zval *) &phasync_ps->mod_user_names;
		zval *dst = (zval *) &st->sess_user_names;

		st->sess_mod = phasync_ps->mod;
		st->sess_status = phasync_ps->mod ? php_session_none : php_session_disabled;
		st->sess_define_sid = 1;
		for (size_t i = 0; i < sizeof(st->sess_user_names) / sizeof(zval); i++) {
			ZVAL_COPY(&dst[i], &src[i]);
		}
		st->sess_user_implemented = phasync_ps->mod_user_implemented;
		if (phasync_ps->mod_user_class_name) {
			st->sess_user_class_name = zend_string_copy(phasync_ps->mod_user_class_name);
		}
# if PHP_VERSION_ID >= 80600
		st->sess_user_obj_methods = phasync_ps->mod_user_uses_object_methods_as_handlers;
# endif
	}
#endif
}

/* The stream behind a body that PHP replaced: php://input streams still read it. */
static void phasync_req_free_body(php_stream *body)
{
	zend_resource *res;
	int le = php_file_le_stream();

	ZEND_HASH_FOREACH_PTR(&EG(regular_list), res) {
		php_stream *s;
		if (res && res->type == le && (s = res->ptr) && s->abstract && s->ops && s->ops->label
		 && strcmp(s->ops->label, "Input") == 0 && *(php_stream **) s->abstract == body) {
			php_stream_free(s, PHP_STREAM_FREE_CLOSE | PHP_STREAM_FREE_KEEP_RSRC);   /* as fclose() */
		}
	} ZEND_HASH_FOREACH_END();
	php_stream_free(body, PHP_STREAM_FREE_CLOSE);
}

static void phasync_req_state_free(phasync_req_state *st)
{
	php_output_handler **h;

	while ((h = zend_stack_top(&st->ob_handlers)) != NULL) {
		php_output_handler_free(h);
		zend_stack_del_top(&st->ob_handlers);
	}
	zend_stack_destroy(&st->ob_handlers);
	if (st->ob_start_file) {
		zend_string_release(st->ob_start_file);
	}
	zend_llist_destroy(&st->headers.headers);
	if (st->headers.mimetype) {
		efree(st->headers.mimetype);
	}
	if (st->headers.http_status_line) {
		efree(st->headers.http_status_line);
	}
#if PHP_VERSION_ID >= 80600
	if (ZEND_FCC_INITIALIZED(st->send_header_fcc)) {
		zend_fcc_dtor(&st->send_header_fcc);
	}
#else
	zval_ptr_dtor(&st->callback_func);
#endif
	if (st->uploaded_files) {           /* deletes the temp files not moved away */
		HashTable *live = SG(rfc1867_uploaded_files);

		SG(rfc1867_uploaded_files) = st->uploaded_files;
		destroy_uploaded_files_hash();
		SG(rfc1867_uploaded_files) = live;
	}
	if (st->request_body) {
		phasync_req_free_body(st->request_body);
	}
	if (st->content_type_dup) {
		efree(st->content_type_dup);
	}
	/* Set from request_info(): owned copies. */
	if (st->request_method) {
		efree((char *) st->request_method);
	}
	if (st->content_type) {
		efree((char *) st->content_type);
	}
	if (st->query_string) {
		efree(st->query_string);
	}
	if (st->request_uri) {
		efree(st->request_uri);
	}
	if (st->cookie_data) {
		efree(st->cookie_data);
	}
	zval_ptr_dtor(&st->if_get);
	zval_ptr_dtor(&st->if_post);
	zval_ptr_dtor(&st->if_cookie);
	zval_ptr_dtor(&st->if_server);
#ifdef PHASYNC_HAVE_SESSION
	zval_ptr_dtor(&st->sess_http_vars);
	if (st->sess_id) {
		zend_string_release(st->sess_id);
	}
	if (st->sess_vars) {
		zend_string_release(st->sess_vars);
	}
	if (st->sess_user_class_name) {
		zend_string_release(st->sess_user_class_name);
	}
	{
		zval *names = (zval *) &st->sess_user_names;

		for (size_t i = 0; i < sizeof(st->sess_user_names) / sizeof(zval); i++) {
			zval_ptr_dtor(&names[i]);
		}
	}
	if (st->sess_started_file) {
		zend_string_release(st->sess_started_file);
	}
#endif
}

static void phasync_req_release(phasync_request *r)
{
	if (--r->refcount) {
		return;
	}
	if (r->bodies_replaced) {
		php_stream *body;

		ZEND_HASH_FOREACH_PTR(r->bodies_replaced, body) {
			phasync_req_free_body(body);
		} ZEND_HASH_FOREACH_END();
		zend_hash_destroy(r->bodies_replaced);
		FREE_HASHTABLE(r->bodies_replaced);
	}
	phasync_req_state_free(&r->st);
	if (r->prebody) {
		php_stream_free(r->prebody, PHP_STREAM_FREE_CLOSE);
	}
	zval_ptr_dtor(&r->server);
	zval_ptr_dtor(&r->sapi);
	zend_string_release(r->name);
	efree(r);
	phasync_req_count--;
}

/* ---- calling the $sapi object, with the process's own state live ---- */

/* Put back an exception set aside around a call: the call's own exception is
 * chained before it, unless it is exit()'s or the unwind of a coroutine being
 * destroyed, which go on. */
static void phasync_req_restore_exception(zend_object *ex)
{
	if (ex == NULL) {
		return;
	}
	if (EG(exception) && !zend_is_unwind_exit(ex) && !zend_is_graceful_exit(ex)) {
		zend_exception_set_previous(EG(exception), ex);
		return;
	}
	if (EG(exception)) {
		OBJ_RELEASE(EG(exception));
	}
	EG(exception) = ex;
}

/* A pending exception (output during unwinding) is set aside for the call. The
 * calling fiber is outside the request meanwhile: if the method switches fibers
 * and back, the request is not installed under it. */
static void phasync_req_call(phasync_request *r, zend_function *fn, zval *rv, uint32_t argc, zval *argv)
{
	zend_ulong key = PHASYNC_REQ_KEY(EG(current_fiber_context));
	zend_object *ex = EG(exception);
	bool member = zend_hash_index_del(&phasync_req_fibers, key) == SUCCESS;
	phasync_request *was_live = phasync_req_cur;

	EG(exception) = NULL;
	phasync_req_install(NULL);
	ZVAL_UNDEF(rv);
	zend_call_known_instance_method(fn, Z_OBJ(r->sapi), rv, argc, argv);
	if (member) {
		zend_hash_index_add_new_ptr(&phasync_req_fibers, key, r);
	}
	phasync_req_install(was_live);
	phasync_req_restore_exception(ex);
}

/* The client is gone: later output is discarded, and, unless ignore_user_abort(),
 * the write that discovered it bails out (as every other SAPI's ub_write does) --
 * r->aborted marks it as ours, so phasync_ext_sapi() unwinds only this request
 * (no catch, no finally, like exit()) instead of letting it end the whole process
 * the way a real fatal error or exit() does. */
static void phasync_req_aborted(phasync_request *r)
{
	r->aborted = true;
	php_handle_aborted_connection();
}

/* ---- the SAPI callbacks, routed to the installed request's $sapi ---- */

/* Output goes to $sapi->write() as a SAPI writes its connection: whole, or the
 * client is gone (false). */
static size_t phasync_req_ub_write(const char *str, size_t len)
{
	phasync_request *r = phasync_req_cur;
	zval arg, rv;

	if (!r) {
		return phasync_req_ub_write_prev(str, len);
	}
	if (r->ended) {
		return len;
	}
	ZVAL_STRINGL_FAST(&arg, str, len);
	phasync_req_call(r, r->m_write, &rv, 1, &arg);
	zval_ptr_dtor(&arg);
	if (Z_TYPE(rv) == IS_FALSE) {
		phasync_req_aborted(r);
	} else if (Z_TYPE(rv) != IS_TRUE && Z_TYPE(rv) != IS_UNDEF) {
		zend_type_error("%s::write() must return a bool, %s returned", ZSTR_VAL(Z_OBJCE(r->sapi)->name), zend_zval_value_name(&rv));
	}
	zval_ptr_dtor(&rv);
	return len;
}

static int phasync_req_send_headers(sapi_headers_struct *h)
{
	phasync_request *r = phasync_req_cur;
	zend_llist_position pos;
	sapi_header_struct *hdr;
	zval args[3], rv;

	if (!r) {
		return phasync_req_send_headers_prev(h);
	}
	if (r->ended) {
		return SAPI_HEADER_SENT_SUCCESSFULLY;
	}
	ZVAL_LONG(&args[0], h->http_response_code);
	if (h->http_status_line) {
		ZVAL_STRING(&args[1], h->http_status_line);
	} else {
		ZVAL_NULL(&args[1]);
	}
	array_init_size(&args[2], zend_llist_count(&h->headers));
	for (hdr = zend_llist_get_first_ex(&h->headers, &pos); hdr; hdr = zend_llist_get_next_ex(&h->headers, &pos)) {
		add_next_index_stringl(&args[2], hdr->header, hdr->header_len);
	}
	phasync_req_call(r, r->m_send_headers, &rv, 3, args);
	zval_ptr_dtor(&args[1]);
	zval_ptr_dtor(&args[2]);
	zval_ptr_dtor(&rv);
	return SAPI_HEADER_SENT_SUCCESSFULLY;
}

static void phasync_req_flush(void *server_context)
{
	phasync_request *r = phasync_req_cur;
	zval rv;

	if (!r) {
		if (phasync_req_flush_prev) {
			phasync_req_flush_prev(server_context);
		}
		return;
	}
	/* A SAPI's flush sends the headers itself (#14): sapi_flush() does not. */
	sapi_send_headers();
	if (r->m_flush && !r->ended) {
		phasync_req_call(r, r->m_flush, &rv, 0, NULL);
		zval_ptr_dtor(&rv);
	}
}

/* Up to len bytes of $sapi->read(), fewer only at the body's end, where read()
 * returns ''. */
static size_t phasync_req_read_sapi(phasync_request *r, char *buf, size_t len)
{
	size_t n = 0;

	while (n < len && !r->ended && r->m_read) {
		zval arg, rv;
		size_t got;

		ZVAL_LONG(&arg, (zend_long) (len - n));
		phasync_req_call(r, r->m_read, &rv, 1, &arg);
		if (Z_TYPE(rv) != IS_STRING) {
			if (Z_TYPE(rv) != IS_UNDEF) {
				zend_type_error("%s::read() must return a string, %s returned", ZSTR_VAL(Z_OBJCE(r->sapi)->name), zend_zval_value_name(&rv));
			}
			zval_ptr_dtor(&rv);
			break;
		}
		got = MIN(Z_STRLEN(rv), len - n);
		memcpy(buf + n, Z_STRVAL(rv), got);
		zval_ptr_dtor(&rv);
		if (got == 0) {
			break;
		}
		n += got;
	}
	return n;
}

/* Like a SAPI's read_post: the body read ahead at startup first, then $sapi->read(). */
static size_t phasync_req_read_post(char *buf, size_t len)
{
	phasync_request *r = phasync_req_cur;
	size_t n = 0;

	if (!r) {
		return phasync_req_read_post_prev ? phasync_req_read_post_prev(buf, len) : 0;
	}
	if (r->body_seen != SG(request_info).request_body) {
		if (r->body_seen) {
			if (!r->bodies_replaced) {
				ALLOC_HASHTABLE(r->bodies_replaced);
				zend_hash_init(r->bodies_replaced, 2, NULL, NULL, 0);
			}
			zend_hash_next_index_insert_ptr(r->bodies_replaced, r->body_seen);
		}
		r->body_seen = SG(request_info).request_body;
	}
	if (r->prebody) {
		ssize_t got = php_stream_read(r->prebody, buf, len);

		n = got > 0 ? (size_t) got : 0;
		if (n == len || r->prebody_all) {
			return n;
		}
	}
	return n + phasync_req_read_sapi(r, buf + n, len - n);
}

/* Headers set in a request are the request's, so the CLI's own handler (which
 * drops them) doesn't see them. */
static int phasync_req_header_handler(sapi_header_struct *h, sapi_header_op_enum op, sapi_headers_struct *hs)
{
	return phasync_req_cur ? SAPI_HEADER_ADD : phasync_req_header_handler_prev(h, op, hs);
}

/* ---- connection_aborted(), connection_status() ---- */

/* A server may know the client left before any write fails. */
static void phasync_req_ask_aborted(void)
{
	phasync_request *r = phasync_req_cur;
	zval rv;

	if (r && r->m_connection_aborted && !r->ended && !(PG(connection_status) & PHP_CONNECTION_ABORTED)) {
		phasync_req_call(r, r->m_connection_aborted, &rv, 0, NULL);
		if (zend_is_true(&rv)) {
			PG(connection_status) |= PHP_CONNECTION_ABORTED;
		}
		zval_ptr_dtor(&rv);
	}
}

static ZEND_NAMED_FUNCTION(phasync_req_connection_aborted_override)
{
	phasync_req_ask_aborted();
	phasync_req_orig_connection_aborted(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

static ZEND_NAMED_FUNCTION(phasync_req_connection_status_override)
{
	phasync_req_ask_aborted();
	phasync_req_orig_connection_status(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* ---- fibers: a fiber belongs to the request of the fiber that starts it ---- */

static void phasync_req_fiber_init(zend_fiber_context *ctx)
{
	phasync_request *r;

	if (phasync_req_count && (r = phasync_req_of(EG(current_fiber_context)))) {
		zend_hash_index_add_new_ptr(&phasync_req_fibers, PHASYNC_REQ_KEY(ctx), r);
		r->refcount++;
	}
}

static void phasync_req_fiber_switch(zend_fiber_context *from, zend_fiber_context *to)
{
	if (phasync_req_count) {
		phasync_req_install(phasync_req_of(to));
	}
}

static void phasync_req_fiber_destroy(zend_fiber_context *ctx)
{
	phasync_request *r;

	if (phasync_req_count && (r = phasync_req_of(ctx))) {
		zend_hash_index_del(&phasync_req_fibers, PHASYNC_REQ_KEY(ctx));
		phasync_req_release(r);
	}
}

/* ---- sapi() ---- */

/* request_info(): ['method' => ..., 'content_type' => ..., 'content_length' => ...,
 * 'query_string' => ..., 'request_uri' => ..., 'cookie' => the Cookie header,
 * 'server' => [...]] describes the request, as a SAPI's request_info and
 * register_server_variables do. */
static void phasync_req_request_info(phasync_request *r, zend_function *fn)
{
	zval rv, *v;

	phasync_req_call(r, fn, &rv, 0, NULL);
	if (Z_TYPE(rv) == IS_ARRAY) {
		if ((v = zend_hash_str_find_deref(Z_ARRVAL(rv), ZEND_STRL("method"))) && Z_TYPE_P(v) == IS_STRING) {
			r->st.request_method = estrndup(Z_STRVAL_P(v), Z_STRLEN_P(v));
			r->st.headers_only = strcasecmp(r->st.request_method, "HEAD") == 0;
		}
		if ((v = zend_hash_str_find_deref(Z_ARRVAL(rv), ZEND_STRL("content_type"))) && Z_TYPE_P(v) == IS_STRING) {
			r->st.content_type = estrndup(Z_STRVAL_P(v), Z_STRLEN_P(v));
		}
		if ((v = zend_hash_str_find_deref(Z_ARRVAL(rv), ZEND_STRL("content_length"))) && Z_TYPE_P(v) == IS_LONG) {
			r->st.content_length = Z_LVAL_P(v);
		}
		if ((v = zend_hash_str_find_deref(Z_ARRVAL(rv), ZEND_STRL("query_string"))) && Z_TYPE_P(v) == IS_STRING) {
			r->st.query_string = estrndup(Z_STRVAL_P(v), Z_STRLEN_P(v));
		}
		if ((v = zend_hash_str_find_deref(Z_ARRVAL(rv), ZEND_STRL("request_uri"))) && Z_TYPE_P(v) == IS_STRING) {
			r->st.request_uri = estrndup(Z_STRVAL_P(v), Z_STRLEN_P(v));
		}
		if ((v = zend_hash_str_find_deref(Z_ARRVAL(rv), ZEND_STRL("cookie"))) && Z_TYPE_P(v) == IS_STRING) {
			r->st.cookie_data = estrndup(Z_STRVAL_P(v), Z_STRLEN_P(v));
		}
		if ((v = zend_hash_str_find_deref(Z_ARRVAL(rv), ZEND_STRL("server"))) && Z_TYPE_P(v) == IS_ARRAY) {
			ZVAL_COPY(&r->server, v);
		}
	}
	zval_ptr_dtor(&rv);
}

#ifdef PHASYNC_HAVE_SESSION
/* The request's end of session, as ext/session's RSHUTDOWN does it: write
 * and close an active session (releasing a files session's flock). Runs
 * with the request still installed (live), before its state is swapped out. */
static void phasync_req_session_end(void)
{
	if (!phasync_ps) {
		return;
	}
	if (phasync_ps->session_status == php_session_active) {
		zend_try {
			php_session_flush(1);
		} zend_end_try();
	}
	zval_ptr_dtor(&phasync_ps->http_session_vars);
	ZVAL_UNDEF(&phasync_ps->http_session_vars);
	if (phasync_ps->mod && (phasync_ps->mod_data || phasync_ps->mod_user_implemented)) {
		zend_try {
			phasync_ps->mod->s_close(&phasync_ps->mod_data);
		} zend_end_try();
	}
	phasync_ps->mod_data = NULL;
	phasync_ps->session_status = php_session_none;
}
#endif

/* The end of a request: the output buffers flushed (removable or not), the
 * headers if no output sent them, and its session written and closed. */
static void phasync_req_finalize(phasync_request *r)
{
	zend_object *ex = EG(exception);

	EG(exception) = NULL;
	php_output_end_all();
	if (!SG(headers_sent)) {
		sapi_send_headers();
	}
#ifdef PHASYNC_HAVE_SESSION
	phasync_req_session_end();
#endif
	php_output_set_status(PHP_OUTPUT_DISABLED);
	r->ended = true;
	phasync_req_restore_exception(ex);
}

static void (*phasync_req_register_server_prev)(zval *track_vars_array);

/* $_SERVER's own entries, as a SAPI's register_server_variables gives them: request_info()'s
 * 'server', each through the input filter as php-fpm's are. The CLI's (the process's
 * environment, the script) are not a web request's. */
static void phasync_req_register_server(zval *track_vars_array)
{
	phasync_request *r = phasync_req_cur;
	zend_string *key;
	zval *v;

	if (!r) {
		if (phasync_req_register_server_prev) {
			phasync_req_register_server_prev(track_vars_array);
		}
		return;
	}
	if (Z_TYPE(r->server) != IS_ARRAY) {
		return;                            /* none given: only what PHP adds itself */
	}
	ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL(r->server), key, v) {
		zend_string *str;

		if (!key) {
			continue;
		}
		str = zval_get_string(v);
		{
			char *val = estrndup(ZSTR_VAL(str), ZSTR_LEN(str));
			size_t len = ZSTR_LEN(str);

			if (!sapi_module.input_filter || sapi_module.input_filter(PARSE_SERVER, ZSTR_VAL(key), &val, len, &len)) {
				php_register_variable_safe(ZSTR_VAL(key), val, len, track_vars_array);
			}
			efree(val);
		}
		zend_string_release(str);
	} ZEND_HASH_FOREACH_END();
}

/* A POST body PHP will parse, read ahead into a temp stream: as far as PHP would read it
 * (none past post_max_size), so parsing it never waits for the client. The rfc1867 parser
 * writes $_FILES into PG(http_globals) as it reads, which a Scope switch while it waited
 * would take away. */
static void phasync_req_prebuffer(phasync_request *r)
{
	zend_long max = SG(post_max_size), cl = SG(request_info).content_length;
	size_t want = SIZE_MAX;
	char buf[8192];

	if (max > 0 && cl > max) {
		return;                            /* PHP warns and reads nothing */
	}
	if (cl > 0) {
		want = (size_t) cl;
	} else if (max > 0) {
		want = (size_t) max + 1;           /* enough for PHP to see it is too large */
	}
	r->prebody = php_stream_temp_create(TEMP_STREAM_DEFAULT, 2 * 1024 * 1024);
	while (want > 0) {
		size_t got = phasync_req_read_sapi(r, buf, MIN(sizeof(buf), want));

		if (got == 0) {
			r->prebody_all = true;
			break;
		}
		php_stream_write(r->prebody, buf, got);
		want -= got;
	}
	if (want == 0 && cl > 0) {
		r->prebody_all = true;             /* the declared length: PHP reads no further */
	}
	php_stream_rewind(r->prebody);
}

/* The request's start, as php_request_startup() does it: a POST form's body read and
 * parsed (sapi_activate()), then the request superglobals created by PHP's own callbacks
 * (php_hash_environment()), all of them now, since compiled code that names one is cached
 * and won't ask again. Errors are reported as "PHP Request Startup". */
static void phasync_req_startup(phasync_request *r)
{
	static const char *const names[] = { "_GET", "_POST", "_COOKIE", "_FILES", "_SERVER", "_REQUEST" };
	static const int tracks[] = { TRACK_VARS_GET, TRACK_VARS_POST, TRACK_VARS_COOKIE, TRACK_VARS_FILES, TRACK_VARS_SERVER, -1 };
	bool during = PG(during_request_startup), bailed = false, post = false;
	zend_execute_data *ex = EG(current_execute_data);

	PG(during_request_startup) = 1;
	if (PG(enable_post_data_reading) && SG(request_info).content_type && SG(request_info).request_method
	 && strcmp(SG(request_info).request_method, "POST") == 0) {
		phasync_req_prebuffer(r);
		post = !EG(exception);
	}
	for (size_t i = 0; i < sizeof(tracks) / sizeof(tracks[0]); i++) {
		if (tracks[i] >= 0) {
			zval_ptr_dtor(&PG(http_globals)[tracks[i]]);
			ZVAL_UNDEF(&PG(http_globals)[tracks[i]]);
		}
	}
	phasync_req_register_server_prev = sapi_module.register_server_variables;
	sapi_module.register_server_variables = phasync_req_register_server;
	EG(current_execute_data) = NULL;      /* no PHP code runs yet: errors are "in Unknown on line 0" */
	/* as at a real request's start, before the modules are activated: the parse of $_POST
	 * runs ext/session's upload progress, which skips it otherwise (request_parse_body()) */
	PG(modules_activated) = false;
	zend_try {
		if (post) {
			sapi_read_post_data();             /* reads the body read ahead: never waits */
		}
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !EG(exception); i++) {
			zend_auto_global *ag = zend_hash_str_find_ptr(CG(auto_globals), names[i], strlen(names[i]));

			if (ag && ag->auto_global_callback) {
				ag->armed = ag->auto_global_callback(ag->name);
			}
		}
	} zend_catch {
		bailed = true;
	} zend_end_try();
	PG(modules_activated) = true;
	EG(current_execute_data) = ex;
	sapi_module.register_server_variables = phasync_req_register_server_prev;
	PG(during_request_startup) = during;
	if (bailed) {
		zend_bailout();
	}
}

ZEND_FUNCTION(phasync_ext_sapi)
{
	zval *sapi, retval, name;
	zend_object *code;
	zend_class_entry *ce;
	phasync_request *r;
	zend_function *m_name, *m_write, *m_request_info;
	zend_fiber_context *ctx = EG(current_fiber_context);
	bool bailed = false;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_OBJ_OF_CLASS(code, zend_ce_closure)
		Z_PARAM_OBJECT(sapi)
	ZEND_PARSE_PARAMETERS_END();

	if (phasync_req_cur) {
		zend_throw_error(NULL, "phasync\\ext\\sapi() cannot be nested");
		RETURN_THROWS();
	}
	ce = Z_OBJCE_P(sapi);
#define PHASYNC_REQ_METHOD(name) zend_hash_str_find_ptr_lc(&ce->function_table, ZEND_STRL(name))
	m_name = PHASYNC_REQ_METHOD("name");
	m_write = PHASYNC_REQ_METHOD("write");
	if (!m_name || !m_write || !PHASYNC_REQ_METHOD("send_headers")) {
		zend_argument_value_error(2, "must have the methods name(), write() and send_headers()");
		RETURN_THROWS();
	}
	r = ecalloc(1, sizeof(*r));
	r->m_write = m_write;
	r->m_read = PHASYNC_REQ_METHOD("read");
	r->m_send_headers = PHASYNC_REQ_METHOD("send_headers");
	r->m_flush = PHASYNC_REQ_METHOD("flush");
	r->m_connection_aborted = PHASYNC_REQ_METHOD("connection_aborted");
	m_request_info = PHASYNC_REQ_METHOD("request_info");
#undef PHASYNC_REQ_METHOD
	r->refcount = 2;                  /* this call + the calling fiber's membership */
	ZVAL_COPY(&r->sapi, sapi);
	phasync_req_state_init(&r->st);
	phasync_req_count++;

	phasync_req_call(r, m_name, &name, 0, NULL);
	if (!EG(exception) && Z_TYPE(name) != IS_STRING) {
		zend_type_error("%s::name() must return a string, %s returned", ZSTR_VAL(ce->name), zend_zval_value_name(&name));
	}
	if (EG(exception)) {
		r->name = ZSTR_EMPTY_ALLOC();
	} else {
		r->name = zend_string_copy(Z_STR(name));
	}
	zval_ptr_dtor(&name);
	if (m_request_info && !EG(exception)) {
		phasync_req_request_info(r, m_request_info);
	}
	zend_hash_index_add_new_ptr(&phasync_req_fibers, PHASYNC_REQ_KEY(ctx), r);
	ZVAL_UNDEF(&retval);
	if (!EG(exception)) {
		phasync_req_install(r);

		/* the default output buffer, as php_request_startup() starts it */
#if PHP_VERSION_ID >= 80600
		if (PG(output_handler) && ZSTR_LEN(PG(output_handler))) {
#else
		if (PG(output_handler) && PG(output_handler)[0]) {
#endif
			zval oh;

#if PHP_VERSION_ID >= 80600
			ZVAL_STR_COPY(&oh, PG(output_handler));
#else
			ZVAL_STRING(&oh, PG(output_handler));
#endif
			php_output_start_user(&oh, 0, PHP_OUTPUT_HANDLER_STDFLAGS);
			zval_ptr_dtor(&oh);
		} else if (PG(output_buffering)) {
			php_output_start_user(NULL, PG(output_buffering) > 1 ? PG(output_buffering) : 0, PHP_OUTPUT_HANDLER_STDFLAGS);
		}
		/* No implicit_flush: the CLI forces it on, a web request has it off (as under
		 * FPM). Output reaches write() at once anyway; flush() runs on flush(). */
		{
			zval fn;

			ZVAL_OBJ(&fn, code);
			/* A bailout must still end the request as phasync_req_finalize() below does --
			 * the installed request and the fiber-membership entry are otherwise left
			 * dangling for whatever runs next (e.g. the CLI's own read_post drain at the
			 * real request's shutdown, issue #121). r->aborted (set by phasync_req_aborted(),
			 * only for a failed write() with ignore_user_abort off) tells this request's own
			 * bailout -- unwinding no catch, no finally, exactly like exit() -- from a real
			 * fatal error or exit() itself: only the latter two continue propagating once this
			 * request is cleaned up, ending the whole process; an aborted request's bailout
			 * stops right here and sapi() returns normally to its caller, leaving every other
			 * concurrently running request (each its own fiber) untouched. */
			zend_try {
				phasync_req_startup(r);
				if (!EG(exception)) {
					call_user_function(NULL, NULL, &fn, &retval, 0, NULL);
				}
			} zend_catch {
				if (!r->aborted) {
					bailed = true;
				}
			} zend_end_try();
		}
		/* finalize() flushes whatever is still buffered: that write can abort too. */
		zend_try {
			phasync_req_finalize(r);
		} zend_catch {
			if (!r->aborted) {
				bailed = true;
			}
		} zend_end_try();
	}
	zend_hash_index_del(&phasync_req_fibers, PHASYNC_REQ_KEY(ctx));
	phasync_req_install(NULL);
	r->ended = true;
	phasync_req_release(r);           /* membership */
	phasync_req_release(r);           /* this call */
	if (bailed) {
		zend_bailout();
	}
	if (EG(exception)) {
		zval_ptr_dtor(&retval);
		RETURN_THROWS();
	}
	if (Z_TYPE(retval) == IS_UNDEF) {
		RETURN_NULL();
	}
	RETURN_COPY_VALUE(&retval);
}

/* ---- module wiring ---- */

static void phasync_req_minit(void)
{
	phasync_req_ub_write_prev = sapi_module.ub_write;
	sapi_module.ub_write = phasync_req_ub_write;
	phasync_req_send_headers_prev = sapi_module.send_headers;
	sapi_module.send_headers = phasync_req_send_headers;
	phasync_req_flush_prev = sapi_module.flush;
	sapi_module.flush = phasync_req_flush;
	phasync_req_read_post_prev = sapi_module.read_post;
	sapi_module.read_post = phasync_req_read_post;
	if ((phasync_req_header_handler_prev = sapi_module.header_handler)) {
		sapi_module.header_handler = phasync_req_header_handler;
	}
	phasync_req_sapi_const = zend_hash_str_find_ptr(EG(zend_constants), ZEND_STRL("PHP_SAPI"));
	/* php_sapi_name() changes inside a request: never evaluated at compile time */
	((zend_function *) zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("php_sapi_name")))->common.fn_flags &= ~ZEND_ACC_COMPILE_TIME_EVAL;
	{
		/* ext/filter's globals: not in its module entry, but filter.default (an
		 * STD_PHP_INI_ENTRY) carries their address, as the INI system uses it */
		zend_ini_entry *e = zend_hash_str_find_ptr(EG(ini_directives), ZEND_STRL("filter.default"));

		if (e && e->mh_arg2) {
#ifdef ZTS
			phasync_if = TSRMG_BULK(*(int *) e->mh_arg2, zend_filter_globals *);
#else
			phasync_if = e->mh_arg2;
#endif
		}
	}
	zend_observer_fiber_init_register(phasync_req_fiber_init);
	zend_observer_fiber_switch_register(phasync_req_fiber_switch);
	zend_observer_fiber_destroy_register(phasync_req_fiber_destroy);
}

static void phasync_req_rinit(void)
{
	zend_internal_function *f;

	zend_hash_init(&phasync_req_fibers, 8, NULL, NULL, 0);
	/* PHP_SAPI changes inside a request, so it is read at run time: no compile-time
	 * or optimizer substitution (CONST_PERSISTENT is put back in RSHUTDOWN, before
	 * the executor's constant cleanup looks at it). */
	ZEND_CONSTANT_SET_FLAGS(phasync_req_sapi_const, ZEND_CONSTANT_FLAGS(phasync_req_sapi_const) & ~CONST_PERSISTENT,
		ZEND_CONSTANT_MODULE_NUMBER(phasync_req_sapi_const));
	CG(compiler_options) |= ZEND_COMPILE_NO_CONSTANT_SUBSTITUTION;
	f = phasync_find_ifunc("connection_aborted", sizeof("connection_aborted") - 1);
	phasync_req_orig_connection_aborted = f->handler;
	f->handler = phasync_req_connection_aborted_override;
	f = phasync_find_ifunc("connection_status", sizeof("connection_status") - 1);
	phasync_req_orig_connection_status = f->handler;
	f->handler = phasync_req_connection_status_override;
}

static void phasync_req_rshutdown(void)
{
	ZEND_CONSTANT_SET_FLAGS(phasync_req_sapi_const, ZEND_CONSTANT_FLAGS(phasync_req_sapi_const) | CONST_PERSISTENT,
		ZEND_CONSTANT_MODULE_NUMBER(phasync_req_sapi_const));
	phasync_find_ifunc("connection_aborted", sizeof("connection_aborted") - 1)->handler = phasync_req_orig_connection_aborted;
	phasync_find_ifunc("connection_status", sizeof("connection_status") - 1)->handler = phasync_req_orig_connection_status;
	zend_hash_destroy(&phasync_req_fibers);
}

/* ---- run_next(): the script again, as a fresh request, in this process ---- */

/* php_module_shutdown() starts with sapi_flush(), after the CLI's request has
 * ended and before anything of the process is taken down: run_next() lives off
 * that call. The process state a request can change but PHP does not undo
 * (cwd, umask, signal mask, alarm) is put back to its start-up value between
 * runs. */
static char *phasync_next_script;               /* malloc'd; one-shot */
static pid_t phasync_next_pid;                  /* the process that recorded it: a forked child that ends does not rerun it */
static bool phasync_next_cli, phasync_next_running;
static void (*phasync_next_flush_prev)(void *server_context);
static char phasync_next_cwd[MAXPATHLEN];
static mode_t phasync_next_umask;
static sigset_t phasync_next_sigmask;
static int phasync_next_status;
static bool phasync_next_title;                 /* do_cli()'s PHP_CLI_PROCESS_TITLE, defined again in every run */
static void phasync_next_flush(void *server_context);
static void phasync_next_exit(void);
static void phasync_next_status_on_exit(void);

/* php_module_shutdown() sets main.c's static module_shutdown, read only by php_verror()
 * to prefix warnings with "PHP Shutdown:". The runs happen inside it, so the flag is
 * cleared while they run. It has no symbol: the first time, each byte that is 1 in the
 * writable data of the object holding php_during_module_shutdown() is set to 0 in turn,
 * and the one that makes php_during_module_shutdown() return false is the flag. Signals
 * are blocked meanwhile and the engine globals skipped (this extension's threads write
 * EG(vm_interrupt)); RELRO is read-only. */
static volatile bool *phasync_next_flag;

#ifdef __ELF__
static bool phasync_next_skip(uintptr_t a)
{
#ifndef ZTS
	if ((a >= (uintptr_t) &executor_globals && a < (uintptr_t) &executor_globals + sizeof(executor_globals))
		|| (a >= (uintptr_t) &compiler_globals && a < (uintptr_t) &compiler_globals + sizeof(compiler_globals))
		|| (a >= (uintptr_t) &core_globals && a < (uintptr_t) &core_globals + sizeof(core_globals))
		|| (a >= (uintptr_t) &sapi_globals && a < (uintptr_t) &sapi_globals + sizeof(sapi_globals))) {
		return true;
	}
#endif
	return false;
}

static int phasync_next_scan(struct dl_phdr_info *info, size_t size, void *data)
{
	uintptr_t fn = (uintptr_t) php_during_module_shutdown, relro_lo = 0, relro_hi = 0;
	bool holds = false;

	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		uintptr_t lo = info->dlpi_addr + ph->p_vaddr;
		if (ph->p_type == PT_LOAD && fn >= lo && fn < lo + ph->p_memsz) {
			holds = true;
		} else if (ph->p_type == PT_GNU_RELRO) {
			relro_lo = lo;
			relro_hi = lo + ph->p_memsz;
		}
	}
	if (!holds) {
		return 0;
	}
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		uintptr_t lo = info->dlpi_addr + ph->p_vaddr;
		if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W)) {
			continue;
		}
		for (uintptr_t a = lo; a < lo + ph->p_memsz; a++) {
			volatile unsigned char *b = (volatile unsigned char *) a;
			if (*b != 1 || (a >= relro_lo && a < relro_hi) || phasync_next_skip(a)) {
				continue;
			}
			*b = 0;
			bool set = php_during_module_shutdown();
			*b = 1;
			if (!set) {
				*(volatile bool **) data = (volatile bool *) b;
				return 1;
			}
		}
	}
	return 1;
}
#endif

static void phasync_next_locate_flag(void)
{
#ifdef __ELF__
	sigset_t all, old;

	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &old);
	dl_iterate_phdr(phasync_next_scan, (void *) &phasync_next_flag);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
#endif
}

static void phasync_next_minit(void)
{
	if (!(phasync_next_cli = strcmp(sapi_module.name, "cli") == 0)) {
		return;
	}
	phasync_next_umask = umask(0);
	umask(phasync_next_umask);
	pthread_sigmask(SIG_SETMASK, NULL, &phasync_next_sigmask);
	if (!getcwd(phasync_next_cwd, sizeof(phasync_next_cwd))) {
		phasync_next_cwd[0] = '\0';
	}
	phasync_next_flush_prev = sapi_module.flush;
	sapi_module.flush = phasync_next_flush;
}

static void phasync_next_mshutdown(void)
{
	if (phasync_next_cli) {
		sapi_module.flush = phasync_next_flush_prev;
	}
	free(phasync_next_script);
	phasync_next_script = NULL;
}

/* do_cli()'s cli_register_file_handles(): the constants are per request. Their
 * streams are dup()s of fd 0-2 (php://std* only hands out the descriptors
 * themselves once per process), closed with the request. */
static void phasync_next_stdio(void)
{
	static const struct { const char *url, *mode, *name; size_t len; } c[] = {
		{"php://stdin", "rb", "STDIN", 5}, {"php://stdout", "wb", "STDOUT", 6}, {"php://stderr", "wb", "STDERR", 6},
	};

	for (int i = 0; i < 3; i++) {
		php_stream *s = php_stream_open_wrapper_ex(c[i].url, c[i].mode, 0, NULL, NULL);
		zend_constant k;

		php_stream_to_zval(s, &k.value);
		ZEND_CONSTANT_SET_FLAGS(&k, CONST_CS, 0);
		k.name = zend_string_init_interned(c[i].name, c[i].len, 0);
		zend_register_constant(&k);
	}
}

static void phasync_next_run(void)
{
	int status = EG(exit_status), original = status;

	if (!phasync_next_flag) {
		phasync_next_locate_flag();
	}
	if (!phasync_next_flag) {
		fprintf(stderr, "phasync\\ext\\run_next(): PHP's module-shutdown flag was not found; the next run is not started\n");
		phasync_next_status = 255;
		phasync_next_status_on_exit();
		return;
	}
	phasync_next_running = true;
	*phasync_next_flag = false;
	zend_interned_strings_switch_storage(1);   /* php_module_shutdown() has switched to the permanent ones */
	while (phasync_next_script) {
		zend_file_handle fh;
		char *script = phasync_next_script;

		phasync_next_script = NULL;
		alarm(0);
		pthread_sigmask(SIG_SETMASK, &phasync_next_sigmask, NULL);
		umask(phasync_next_umask);
		if (phasync_next_cwd[0] && chdir(phasync_next_cwd) != 0) {
			fprintf(stderr, "phasync\\ext\\run_next(): cannot return to %s: %s\n", phasync_next_cwd, strerror(errno));
			status = 255;
			free(script);
			break;
		}
		/* do_cli() points path_translated at a copy it frees after its request; sapi_activate() leaves the code alone */
		SG(request_info).path_translated = script;
		SG(sapi_headers).http_response_code = 0;
		if (php_request_startup() == FAILURE) {
			fprintf(stderr, "phasync\\ext\\run_next(): php_request_startup() failed\n");
			status = 255;
			free(script);
			break;
		}
		EG(exit_status) = 0;
		phasync_next_stdio();
		/* as do_cli() after its php_request_startup() */
		CG(skip_shebang) = 1;
		zend_register_bool_constant(ZEND_STRL("PHP_CLI_PROCESS_TITLE"), phasync_next_title, 0, 0);
		zend_is_auto_global(ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_SERVER));
		/* a FILE* like do_cli's, not a stream resource: the request looks like a first one */
		FILE *fp = fopen(script, "rb");
		if (fp) {
			zend_stream_init_fp(&fh, fp, script);
			fh.primary_script = 1;
			php_execute_script(&fh);
			zend_destroy_file_handle(&fh);
		} else {
			fprintf(stderr, "Could not open input file: %s\n", script);
			EG(exit_status) = 1;
		}
		php_request_shutdown(NULL);
		status = EG(exit_status);
		free(script);
	}
	SG(request_info).path_translated = NULL;
	zend_interned_strings_switch_storage(0);
	*phasync_next_flag = true;
	phasync_next_running = false;
	if (status != original) {
		phasync_next_status = status;
		phasync_next_status_on_exit();
	}
}

/* main() returns do_cli()'s status: phasync_next_status is put in its place once everything else has shut down */
static void phasync_next_status_on_exit(void)
{
	/* an atexit handler of this library runs when it is unloaded, in the middle of the module shutdown (and the code is gone
	 * by exit): one more reference keeps it loaded until the process ends */
	Dl_info info;
	dladdr((void *)phasync_next_exit, &info);
	dlopen(info.dli_fname, RTLD_NOW);
	atexit(phasync_next_exit);
}

static void phasync_next_exit(void)
{
	fflush(NULL);
	_exit(phasync_next_status);
}

static void phasync_next_flush(void *server_context)
{
	phasync_next_flush_prev(server_context);
	if (php_during_module_shutdown() && !phasync_next_running && phasync_next_script && phasync_next_pid == getpid()) {
		phasync_next_run();
	}
}

ZEND_FUNCTION(phasync_ext_run_next)
{
	zend_string *script;
	char resolved[MAXPATHLEN];
	struct stat st;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_PATH_STR(script)
	ZEND_PARSE_PARAMETERS_END();

	if (!phasync_next_cli) {
		zend_throw_error(NULL, "phasync\\ext\\run_next() requires the CLI SAPI (this is %s)", sapi_module.name);
		RETURN_THROWS();
	}
#ifndef __ELF__
	zend_throw_error(NULL, "phasync\\ext\\run_next() is not supported on this platform");
	RETURN_THROWS();
#endif
	if (!expand_filepath(ZSTR_VAL(script), resolved) || stat(resolved, &st) != 0 || !S_ISREG(st.st_mode) || access(resolved, R_OK) != 0) {
		zend_argument_value_error(1, "must be a readable file, \"%s\" is not", resolved);
		RETURN_THROWS();
	}
	free(phasync_next_script);
	phasync_next_script = strdup(resolved);
	phasync_next_pid = getpid();
	zval *title = zend_get_constant_str(ZEND_STRL("PHP_CLI_PROCESS_TITLE"));
	phasync_next_title = title && zend_is_true(title);
}

/* ---- dup2(): a stream's fd onto a given descriptor, outliving the request ---- */

ZEND_FUNCTION(phasync_ext_dup2)
{
	zval *zstream;
	zend_long fd;
	php_stream *stream;
	php_socket_t src;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_RESOURCE(zstream)
		Z_PARAM_LONG(fd)
	ZEND_PARSE_PARAMETERS_END();

	if (fd < 0) {
		zend_argument_value_error(2, "must not be negative");
		RETURN_THROWS();
	}
	php_stream_from_zval(stream, zstream);
	if ((src = phasync_stream_fd(stream)) == -1) {
		zend_argument_type_error(1, "must be a stream with a file descriptor");
		RETURN_THROWS();
	}
	if (src != (php_socket_t) fd && dup2(src, (int) fd) == -1) {
		zend_throw_exception_ex(spl_ce_RuntimeException, 0, "dup2(%d, %d) failed: %s", (int) src, (int) fd, strerror(errno));
		RETURN_THROWS();
	}
}
