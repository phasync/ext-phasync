/* phasync\ext\sapi(): gives each coroutine its own request.
 *
 * The SAPI-facing side of a web server written in PHP: while a coroutine runs
 * inside sapi(), PHP's output layer (output buffers, headers, the response
 * status), its request (method, query string, body behind php://input,
 * uploaded files) and the session are the coroutine's own, swapped in and out
 * when fibers switch. Fibers started inside belong to the request of the
 * fiber that starts them. What would go to the client goes to the $sapi
 * object, as a SAPI would receive it. sapi() knows nothing of Scope and
 * touches no superglobal. Nothing else is isolated: not statics, declarations,
 * other globals, shutdown functions or error handlers. exit() is not
 * intercepted.
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
	X(connection_status,  PG(connection_status)) \
	X(ignore_user_abort,  PG(ignore_user_abort))

typedef struct phasync_req_state {
#define PHASYNC_REQ_DECL(name, live) __typeof__(live) name;
	PHASYNC_REQ_STATE(PHASYNC_REQ_DECL)
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
	zend_string *name;                 /* PHP_SAPI and php_sapi_name() while installed */
	zval sapi, out, in;
	zend_function *m_send_headers, *m_flush, *m_connection_aborted;
	phasync_req_state st;              /* the live globals while the request is not installed */
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
			ZVAL_COPY_VALUE(&phasync_req_cli_val, &phasync_req_sapi_const->value);
		}
		sapi_module.name = ZSTR_VAL(in->name);
		ZVAL_STR(&phasync_req_sapi_const->value, in->name);
	} else {
		sapi_module.name = phasync_req_cli_name;
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
	zval_ptr_dtor(&r->sapi);
	zval_ptr_dtor(&r->out);
	zval_ptr_dtor(&r->in);
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

/* The client is gone: later output is discarded. The code is not stopped. */
static void phasync_req_aborted(void)
{
	PG(connection_status) = PHP_CONNECTION_ABORTED;
	php_output_set_status(PHP_OUTPUT_DISABLED);
}

/* The stream a $sapi->output()/input() zval holds; NULL once the server closed it. */
static php_stream *phasync_req_stream(zval *zv)
{
	return Z_TYPE_P(zv) == IS_RESOURCE
		? zend_fetch_resource2(Z_RES_P(zv), NULL, php_file_le_stream(), php_file_le_pstream())
		: NULL;
}

/* ---- the SAPI callbacks, routed to the installed request's $sapi ---- */

/* Output goes to $sapi->output() as a SAPI writes its connection: whole, or the
 * client is gone. A failed write raises no notice of the stream's own. */
static size_t phasync_req_ub_write(const char *str, size_t len)
{
	phasync_request *r = phasync_req_cur;
	php_stream *s;
	ssize_t n;
	uint32_t flags;

	if (!r) {
		return phasync_req_ub_write_prev(str, len);
	}
	if (r->ended) {
		return len;
	}
	if (!(s = phasync_req_stream(&r->out))) {
		phasync_req_aborted();
		return len;
	}
	flags = s->flags;
	s->flags |= PHP_STREAM_FLAG_SUPPRESS_ERRORS;
	n = php_stream_write(s, str, len);
	s->flags = (s->flags & ~PHP_STREAM_FLAG_SUPPRESS_ERRORS) | (flags & PHP_STREAM_FLAG_SUPPRESS_ERRORS);
	if (n < 0 || (size_t) n < len) {
		phasync_req_aborted();
	}
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
	php_stream *s;
	zval rv;

	if (!r) {
		if (phasync_req_flush_prev) {
			phasync_req_flush_prev(server_context);
		}
		return;
	}
	/* A SAPI's flush sends the headers itself (#14): sapi_flush() does not. */
	sapi_send_headers();
	if (!r->ended && (s = phasync_req_stream(&r->out))) {
		php_stream_flush(s);
	}
	if (r->m_flush && !r->ended) {
		phasync_req_call(r, r->m_flush, &rv, 0, NULL);
		zval_ptr_dtor(&rv);
	}
}

/* Like a SAPI's read_post: up to len bytes of $sapi->input(), fewer only at its end. */
static size_t phasync_req_read_post(char *buf, size_t len)
{
	phasync_request *r = phasync_req_cur;
	php_stream *s;
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
	while (n < len && !r->ended && (s = phasync_req_stream(&r->in))) {
		uint32_t flags = s->flags;
		ssize_t got;

		s->flags |= PHP_STREAM_FLAG_SUPPRESS_ERRORS;
		got = php_stream_read(s, buf + n, len - n);
		s->flags = (s->flags & ~PHP_STREAM_FLAG_SUPPRESS_ERRORS) | (flags & PHP_STREAM_FLAG_SUPPRESS_ERRORS);
		if (got <= 0) {
			break;
		}
		n += got;
	}
	return n;
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

/* $sapi->output() and $sapi->input(): the body streams, asked for once. */
static void phasync_req_streams(phasync_request *r, zend_function *m_output, zend_function *m_input)
{
	phasync_req_call(r, m_output, &r->out, 0, NULL);
	if (!EG(exception) && m_input) {
		phasync_req_call(r, m_input, &r->in, 0, NULL);
	}
	if (EG(exception)) {
		return;
	}
	if (!phasync_req_stream(&r->out)) {
		zend_type_error("%s::output() must return a stream, %s returned", ZSTR_VAL(Z_OBJCE(r->sapi)->name), zend_zval_value_name(&r->out));
	} else if (m_input && Z_TYPE(r->in) != IS_NULL && !phasync_req_stream(&r->in)) {
		zend_type_error("%s::input() must return a stream or null, %s returned", ZSTR_VAL(Z_OBJCE(r->sapi)->name), zend_zval_value_name(&r->in));
	}
}

/* request_info(): ['method' => ..., 'content_type' => ..., 'content_length' => ...,
 * 'query_string' => ..., 'request_uri' => ...] describes the request, as a SAPI's
 * request_info does. */
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

ZEND_FUNCTION(phasync_ext_sapi)
{
	zval *sapi, retval, name;
	zend_object *code;
	zend_class_entry *ce;
	phasync_request *r;
	zend_function *m_name, *m_output, *m_input, *m_request_info;
	zend_fiber_context *ctx = EG(current_fiber_context);

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
	m_output = PHASYNC_REQ_METHOD("output");
	m_input = PHASYNC_REQ_METHOD("input");
	if (!m_name || !m_output || !PHASYNC_REQ_METHOD("send_headers")) {
		zend_argument_value_error(2, "must have the methods name(), output() and send_headers()");
		RETURN_THROWS();
	}
	r = ecalloc(1, sizeof(*r));
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
	if (!EG(exception)) {
		phasync_req_streams(r, m_output, m_input);
	}
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
		} else if (PG(implicit_flush)) {
			php_output_set_implicit_flush(1);
		}
		{
			zval fn;

			ZVAL_OBJ(&fn, code);
			call_user_function(NULL, NULL, &fn, &retval, 0, NULL);
		}
		phasync_req_finalize(r);
	}
	zend_hash_index_del(&phasync_req_fibers, PHASYNC_REQ_KEY(ctx));
	phasync_req_install(NULL);
	r->ended = true;
	phasync_req_release(r);           /* membership */
	phasync_req_release(r);           /* this call */
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
