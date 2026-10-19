/* Included by phasync.c (single translation unit); not compiled on its own. */

/* Non-blocking I/O on streams, sockets and pipes: stream/socket/pipe wrapping, stdio and php://input, preadv2()/RWF_NOWAIT, the wrapped-ops machinery shared across socket and pipe originals. */


/* ---- fd + wait helpers --------------------------------------------------- */

static php_socket_t phasync_stream_fd(php_stream *stream)
{
	php_socket_t fd = -1;
	php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL,
		(void *) &fd, 0);
	return fd;
}

/* PHP destroys a fiber dropped while suspended by resuming it once to unwind it:
 * its finally blocks and destructors run, and it can no longer suspend
 * (Fiber::suspend() throws). A wait there must not reach the loop at all. */
static zend_always_inline bool phasync_unwinding(void)
{
	return EG(active_fiber) && (EG(active_fiber)->flags & ZEND_FIBER_FLAG_DESTROYED);
}

/* Fail a wait in a fiber being destroyed as Fiber::suspend() would, unless the
 * unwind (or another exception) is already pending. */
static void phasync_throw_force_closed(void)
{
	if (!EG(exception)) {
		zend_throw_error(zend_hash_str_find_ptr(CG(class_table), ZEND_STRL("fibererror")),
			"Cannot suspend in a force-closed fiber");
	}
}

static bool phasync_cannot_suspend(void)
{
	if (!phasync_unwinding()) {
		return false;
	}
	phasync_throw_force_closed();
	return true;
}

/* Call the sleep handler as handler(int $microseconds); -1 if it threw. */
static int phasync_call_sleep(zval *handler, zend_long usec)
{
	zval arg, retval;
	int rc = 0;

	if (phasync_cannot_suspend()) {
		return -1;
	}
	ZVAL_LONG(&arg, usec);
	ZVAL_UNDEF(&retval);
	if (call_user_function(NULL, NULL, handler, &retval, 1, &arg) == FAILURE || EG(exception)) {
		rc = -1;
	}
	zval_ptr_dtor(&retval);
	return rc;
}

/* If the stream is a socket, return its netstream data — which holds the timeout
 * and the timed-out flag — otherwise NULL. xp_socket's udp/unix/udg streams share
 * php_sockop_read as their read op. But ext/openssl registers itself for tcp://
 * too (so crypto can be enabled on a plain TCP stream later), so TCP and ssl/tls
 * streams use openssl's static ops, recognisable only by their label; its stream
 * data starts with a php_netstream_data_t (xp_ssl.c), so the cast holds. */
static php_netstream_data_t *phasync_sock_data(php_stream *stream)
{
	const php_stream_ops *orig = stream ? PHASYNC_ORIG(stream) : NULL;
	if (orig && stream->abstract
	 && (orig->read == php_stream_socket_ops.read
	  || (orig->label && strcmp(orig->label, "tcp_socket/ssl") == 0))) {
		return (php_netstream_data_t *) stream->abstract;
	}
	return NULL;
}

/* The timeout (seconds) native PHP would apply to a would-block wait here: the
 * socket's own timeout (stream_set_timeout()/default_socket_timeout; tv_sec == -1
 * means none), or INF for non-sockets (pipes/FIFOs/files block forever). */
static double phasync_wait_timeout(php_stream *stream)
{
	php_netstream_data_t *sock = phasync_sock_data(stream);
	if (sock == NULL || sock->timeout.tv_sec == -1) {
		return INFINITY;
	}
	return (double) sock->timeout.tv_sec + (double) sock->timeout.tv_usec / 1000000.0;
}

/* Flag the stream as timed out exactly as the native socket op does, so
 * stream_get_meta_data()['timed_out'] === true. No-op for non-sockets. */
static void phasync_mark_timed_out(php_stream *stream)
{
	php_netstream_data_t *sock = phasync_sock_data(stream);
	if (sock) {
		sock->timeout_event = true;
	}
}

/* Since 8.3, native php_sockop_read does not wait at all (MSG_DONTWAIT, returns 0)
 * when the current read call has already delivered buffered data, or when the
 * socket's timeout is exactly zero. Mirror that for sockets; pipes/files block
 * natively. 8.2 has no such rule: a blocking read always waits, and a zero timeout
 * times out at once — which the normal wait path already reproduces. */
static bool phasync_read_dont_wait(php_stream *stream)
{
	php_netstream_data_t *sock = phasync_sock_data(stream);
	return sock && (stream->has_buffered_data
		|| (sock->timeout.tv_sec == 0 && sock->timeout.tv_usec == 0));
}

/* Native php_sockop_write reports a timed-out send with a notice; mirror it. */
static void phasync_write_timeout_notice(php_stream *stream, size_t count, int err)
{
	char *estr;

	if (!phasync_sock_data(stream) || (stream->flags & PHP_STREAM_FLAG_SUPPRESS_ERRORS)) {
		return;
	}
	estr = php_socket_strerror(err, NULL, 0);
#ifdef php_stream_warn
	php_stream_warn(stream, NetworkSendFailed,
		"Send of %zu bytes failed with errno=%d %s", count, err, estr);
#else
	php_error_docref(NULL, E_NOTICE,
		"Send of %zu bytes failed with errno=%d %s", count, err, estr);
#endif
	efree(estr);
}

static double phasync_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

/* ---- a stream closed under a suspended coroutine (#14) --------------------
 *
 * A coroutine suspended inside an operation on a stream still has the stream on
 * its C stack: in our op, in PHP's stream layer, in mysqlnd (with the connection
 * mysqli frees as it closes). Another coroutine closing the stream then must not
 * free it before they have left: its close op wakes them, parks the closer until
 * the last one is out, and only then lets the close go on. A woken operation fails
 * as on a closed descriptor (EBADF), and so does any later one that would wait on
 * the closing stream. PHP frees a stream as its close op returns, whatever the
 * path (fclose(), mysqli::close(), ...), so the close op is where it waits.
 *
 * The closer's fiber is held alive while it waits: destroyed there, it could
 * neither wait nor keep the stream from being freed under the others (#21). Held,
 * it is resumed by its loop once they are out, or, if its loop has let it go, left
 * to PHP to destroy once they are (phasync_holds_release()).
 *
 * A close beginning in a coroutine that is already unwinding (its finally
 * blocks) cannot wait this way at all: Fiber::suspend() throws there. For the
 * three adapters that route through here -- fclose() (in all its forms, the
 * resource destructor and proc_close()/pclose() pipes included) -- such a close
 * wakes the waiters and marks the guard closing exactly as above, but skips the
 * real free entirely instead of waiting for it to become safe
 * (phasync_close_if_unwinding(), #23): the handle is left open, to whichever
 * reference outlives the others (each waiter holds its own for the duration of
 * its operation), and closes for real, through this same code, once that
 * reference is the last one standing. mysqli::close() has no such adapter: it
 * stays a fatal error (frees mysqlnd's own structures outside the streams API;
 * see the README). */

/* ---- the guard's core: reusable for any handle kind, keyed by its address --
 *
 * phasync_inflight_enter()/_out()/_leave() are the whole of what a new adapter
 * needs from the guard for the wait side (park a coroutine "in flight" on a
 * handle, and settle it back out): they take the handle only as an opaque
 * (void *) key, with no php_stream-specific code in them. The close side
 * (phasync_inflight_close()/_drain(), phasync_close_if_unwinding()) stays
 * typed to php_stream *, since waking a handle's waiters (phasync_stream_forget())
 * is necessarily specific to the kind of handle; a future adapter (ext/sockets'
 * socket_close(), say) writes its own small close-side wrapper around the same
 * enter/out/leave, the way the stream adapter below does. */

/* Before suspending in an op on the handle. False (errno EBADF) if it is closing. */
static bool phasync_inflight_enter(void *handle)
{
	phasync_inflight *f = zend_hash_index_find_ptr(&PHASYNC_G(inflight), (zend_ulong) (uintptr_t) handle);

	if (f == NULL) {
		f = pecalloc(1, sizeof(*f), 1);
		zend_hash_index_add_new_ptr(&PHASYNC_G(inflight), (zend_ulong) (uintptr_t) handle, f);
	} else if (f->closing) {
		errno = EBADF;
		return false;
	}
	f->waiters++;
	phasync_ledger_cur()->inflight = handle;
	return true;
}

/* One coroutine less inside an op on the handle; the last one out of a closing
 * one lets the closer go on. True if it is closing. */
static bool phasync_inflight_out(void *handle)
{
	phasync_inflight *f = zend_hash_index_find_ptr(&PHASYNC_G(inflight), (zend_ulong) (uintptr_t) handle);

	if (--f->waiters == 0) {
		if (!f->closing) {
			zend_hash_index_del(&PHASYNC_G(inflight), (zend_ulong) (uintptr_t) handle);
			return false;
		}
		if (f->closer) {
			phasync_chan_push(f->closer->chan, f->closer_slot);
		}
	}
	return f->closing;
}

/* Back from the suspension. True (errno EBADF) if the handle is closing: the op fails. */
static bool phasync_inflight_leave(void *handle)
{
	phasync_ledger_cur()->inflight = NULL;
	if (phasync_inflight_out(handle)) {
		errno = EBADF;
		return true;
	}
	return false;
}

/* Let go of a closer's fiber once it no longer waits. Resumed by its loop, the
 * loop holds it too; if only we do, it is running and must not be freed under
 * itself: then the reference is left to the end of the request. */
static void phasync_hold_drop(phasync_inflight *f)
{
	zend_object *o = f->hold;

	f->hold = NULL;
	PHASYNC_G(holds)--;
	if (GC_REFCOUNT(o) > 1) {
		GC_DELREF(o);
	}
}

/* Where no coroutine is inside an op (a Poller's poll(), a manage() starting): let
 * PHP have the closers held for streams nobody is inside any more. One whose loop
 * let it go is destroyed here, and its close ends as it unwinds. */
static void phasync_holds_release(void)
{
	zend_object *release[16];
	phasync_inflight *f;
	int n = 0;

	if (PHASYNC_G(holds) == 0) {
		return;
	}
	ZEND_HASH_FOREACH_PTR(&PHASYNC_G(inflight), f) {
		if (f->hold && f->waiters == 0 && n < 16) {
			release[n++] = f->hold;
			f->hold = NULL;
			PHASYNC_G(holds)--;
		}
	} ZEND_HASH_FOREACH_END();
	for (int i = 0; i < n; i++) {
		OBJ_RELEASE(release[i]);
	}
}

/* The request is ending and PHP destroys every object: a closer destroyed while
 * coroutines are still inside an op on its stream cannot wait for them, but they
 * are about to be destroyed too. Destroy them first, as PHP would. */
static void phasync_inflight_destroy_waiters(php_stream *stream)
{
	zend_object *victims[16];
	phasync_ledger *l;
	zend_ulong key;
	int n = 0;

	ZEND_HASH_FOREACH_NUM_KEY_PTR(&PHASYNC_G(ledgers), key, l) {
		zend_fiber_context *ctx = (zend_fiber_context *) (uintptr_t) key;
		if (l->inflight == stream && ctx->kind == zend_ce_fiber && n < 16) {
			zend_object *obj = &((zend_fiber *) ((char *) ctx - offsetof(zend_fiber, context)))->std;
			if (!(OBJ_FLAGS(obj) & IS_OBJ_DESTRUCTOR_CALLED)) {
				GC_ADDREF(obj);
				victims[n++] = obj;
			}
		}
	} ZEND_HASH_FOREACH_END();
	for (int i = 0; i < n; i++) {
		GC_ADD_FLAGS(victims[i], IS_OBJ_DESTRUCTOR_CALLED);
		victims[i]->handlers->dtor_obj(victims[i]);
		OBJ_RELEASE(victims[i]);
	}
}

/* Wake whoever waits on the closing stream and park until every coroutine
 * suspended in an op on it has left. A cancellation of the wait is held until
 * they are out. False if the closer is being destroyed while they are still in
 * (it began closing while unwinding): it cannot wait, and the unwind is pending. */
static bool phasync_inflight_drain(php_stream *stream, phasync_inflight *f)
{
	phasync_poller *p = PHASYNC_G(scope_top)->poller;
	phasync_ledger *l = phasync_ledger_cur();
	zend_object *held = NULL;
	bool destroyed_waiters = false;

	phasync_stream_forget(stream);
	f->closing = true;
	f->closer = p;
	GC_ADDREF(&p->std);
	l->closing = stream;
	l->closing_p = p;
	if (!phasync_unwinding()) {
		f->hold = &EG(active_fiber)->std;
		GC_ADDREF(f->hold);
		PHASYNC_G(holds)++;
	}
	while (f->waiters) {
		if (phasync_unwinding()) {
			if ((EG(flags) & EG_FLAGS_IN_SHUTDOWN) && !destroyed_waiters) {
				destroyed_waiters = true;
				phasync_inflight_destroy_waiters(stream);
				continue;
			}
			break;                   /* the unwind stays pending */
		}
		if ((f->closer_slot = phasync_get_slot(p)) < 0
		 || phasync_park(p, f->closer_slot, INFINITY, NULL) != PHASYNC_WAIT_READY) {
			if (EG(exception) == NULL) {
				zend_error_noreturn(E_ERROR, "phasync: unable to wait for the coroutines using a stream being closed");
			}
			if (phasync_unwinding()) {
				continue;
			}
			if (held == NULL) {
				held = EG(exception);
				GC_ADDREF(held);
			}
			zend_clear_exception();
		}
	}
	l->closing = NULL;
	f->closer = NULL;
	OBJ_RELEASE(&p->std);
	if (f->hold) {
		phasync_hold_drop(f);
	}
	if (f->waiters) {
		if (held) {
			OBJ_RELEASE(held);
		}
		return false;
	}
	if (held) {
		zend_throw_exception_internal(held);
	}
	return true;
}

/* From a close op, before the stream goes: wait until no coroutine is inside an op
 * on it (see phasync_inflight_drain()). The closer can only wait in a coroutine;
 * anywhere else it would free the stream under them, so that is a fatal error. */
static void phasync_inflight_close(php_stream *stream)
{
	phasync_inflight *f = zend_hash_index_find_ptr(&PHASYNC_G(inflight), (zend_ulong) (uintptr_t) stream);

	phasync_stream_forget(stream);
	if (f == NULL) {
		return;
	}
	if (f->waiters) {
		if (PHASYNC_G(scope_top) == NULL || EG(active_fiber) == NULL) {
			zend_error_noreturn(E_ERROR, "phasync: a stream was closed outside a coroutine while %u coroutine(s) wait on it",
				f->waiters);
		}
		if (!phasync_inflight_drain(stream, f)) {
			zend_error_noreturn(E_ERROR, "phasync: a stream was closed by a coroutine being destroyed while %u coroutine(s) wait on it",
				f->waiters);
		}
	}
	zend_hash_index_del(&PHASYNC_G(inflight), (zend_ulong) (uintptr_t) stream);
}

/* An adapter's entry point for a close beginning in a coroutine that is already
 * unwinding (#23): it cannot suspend, so phasync_inflight_drain()'s wait is not
 * an option. If nothing is inside an op on the stream there is nothing to do
 * here: the caller's normal close goes on exactly as without the extension.
 * Otherwise: wake the waiters and mark the guard closing (their op fails,
 * EBADF), but do not free the stream at all -- leave it open under whichever
 * reference outlives the others (each waiter holds its own for the duration of
 * its operation; see the top of this section). Its close then goes through this
 * same guard for real, once that reference is the last one, by which point
 * nothing is left to wait for. True: the caller must treat the close as done
 * and must not call the real one. */
static bool phasync_close_if_unwinding(php_stream *stream)
{
	phasync_inflight *f;

	if (!phasync_unwinding()) {
		return false;
	}
	f = zend_hash_index_find_ptr(&PHASYNC_G(inflight), (zend_ulong) (uintptr_t) stream);
	if (f == NULL || f->waiters == 0) {
		return false;
	}
	phasync_stream_forget(stream);
	f->closing = true;
	return true;
}

/* The hooked operations wait through the manage() scope's Poller, with its
 * timeout class. A wait on a stream fails with CLOSED if another coroutine
 * closes it meanwhile. There is no concurrency cap: every hooked wait goes on
 * this same cooperative path, so a stream whose readiness depends on a SIBLING
 * coroutine of the same worker can never be starved by this one. */
static int phasync_wait_fd(int dir, php_stream *stream, php_socket_t fd, double timeout)
{
	phasync_scope *s = PHASYNC_G(scope_top);
	int rc;

	if (stream != NULL && !phasync_inflight_enter(stream)) {
		return PHASYNC_WAIT_CLOSED;
	}
	rc = phasync_poller_wait(s->poller, dir, stream, fd, timeout, s->timeout_ce, false);
	if (stream == NULL) {
		return rc;
	}
	return phasync_inflight_leave(stream) && rc != PHASYNC_WAIT_ERROR ? PHASYNC_WAIT_CLOSED : rc;
}

#define PHASYNC_COOP_ERROR    0   /* stop: exception pending          */
#define PHASYNC_COOP_RETRY    1   /* ready: retry the op              */
#define PHASYNC_COOP_TIMEOUT  2   /* stop: native timeout path taken  */

/* One cooperative wait inside a read/write op. *deadline carries the wait's
 * deadline across retries within one op call (one fill), so a spurious wake-up
 * is handed only the REMAINING time; callers start each op call with
 * *deadline < 0, giving every fill a fresh full timeout, as
 * php_sock_stream_wait_for_data does. On timeout the stream's timed-out flag is
 * set, exactly as the native socket op sets it. */
static int phasync_cooperate(int dir, php_stream *stream, php_socket_t fd, double *deadline)
{
	double timeout = phasync_wait_timeout(stream), wait = INFINITY;
	int w;

	if (!isinf(timeout)) {
		double now = phasync_now();
		if (*deadline < 0) {
			*deadline = now + timeout;
			wait = timeout;                /* first wait of this fill: the exact native timeout */
		} else {
			wait = *deadline - now;        /* a retry: only the remaining time */
		}
		if (wait <= 0) {                   /* time used up (or a zero timeout) */
			phasync_mark_timed_out(stream);
			return PHASYNC_COOP_TIMEOUT;
		}
	}
	w = phasync_wait_fd(dir, stream, fd, wait);
	if (w == PHASYNC_WAIT_READY) {
		return PHASYNC_COOP_RETRY;
	}
	if (w == PHASYNC_WAIT_TIMEOUT) {
		phasync_mark_timed_out(stream);
		return PHASYNC_COOP_TIMEOUT;
	}
	return PHASYNC_COOP_ERROR;
}

/* ---- wrapped stream ops (shared across socket + pipe originals) ----------- */

static phasync_hook_entry *phasync_entry(php_stream *stream)
{
	return zend_hash_index_find_ptr(&PHASYNC_G(hooked), (zend_ulong) (uintptr_t) stream);
}

static phasync_hook_entry *phasync_entry_ensure(php_stream *stream)
{
	phasync_hook_entry *e = phasync_entry(stream);
	if (e == NULL) {
		e = pemalloc(sizeof(*e), 1);
		e->pidfd = -1;
		zend_hash_index_add_ptr(&PHASYNC_G(hooked), (zend_ulong) (uintptr_t) stream, e);
	}
	return e;
}

/* Nothing about a stream's blocking mode is cached: every hooked op reads the
 * fd's real O_NONBLOCK state fresh, with fcntl(F_GETFL), right before deciding
 * whether to cooperate. Returns -1 (treat as "can't tell, do the native op") when
 * fd is -1 or F_GETFL fails. */
static int phasync_fd_flags(php_socket_t fd)
{
	return fd == -1 ? -1 : fcntl(fd, F_GETFL, 0);
}

#if PHASYNC_HAVE_NOWAIT
/* issue #42: RWF_NOWAIT support turns out to depend on the fd's DEVICE, not
 * just the kernel version -- confirmed empirically on this very machine: an
 * anonymous pipe (pipe(2), what proc_open()'s pipes to a Lua/Scribunto child
 * actually are) accepts preadv2()/pwritev2() with RWF_NOWAIT; a named FIFO
 * on an ordinary filesystem (open(2) on a mkfifo()'d path, exercised by
 * tests/pool_fifo_rendezvous.phpt) answers EOPNOTSUPP for the exact same
 * call on the exact same kernel. So, same shape as phasync_fs_fd_mode()'s
 * own nowait_off list above (for plain files; a FIFO on the SAME device has
 * been seen to disagree with its own files, so this is its own, separate
 * list): remember which devices refuse it, and skip trying again on that
 * device. While none has ever refused (the overwhelmingly common case: a
 * worker that only ever talks to anonymous pipes never adds anything here),
 * this costs no extra syscall at all -- the comparison is a single integer
 * check, done before the fstat() an actual lookup would need. */
static bool phasync_pipe_dev_blocked(php_socket_t fd)
{
	struct stat st;

	if (PHASYNC_G(npipe_nowait_off) == 0) {
		return false;
	}
	if (fstat(fd, &st) != 0) {
		return false;   /* can't tell -- try it, same as a cache miss */
	}
	for (int i = 0; i < PHASYNC_G(npipe_nowait_off); i++) {
		if (PHASYNC_G(pipe_nowait_off)[i] == st.st_dev) {
			return true;
		}
	}
	return false;
}

/* Records that this fd's device just answered EOPNOTSUPP/EINVAL/ENOSYS to
 * RWF_NOWAIT, so later pipes/FIFOs on the same device skip straight to the
 * fcntl-toggle fallback instead of paying for the same failed syscall again.
 * Best-effort: a failed fstat() or realloc() just means it tries again next
 * time, never a correctness problem (the fallback below always runs either
 * way). */
static void phasync_pipe_dev_block(php_socket_t fd)
{
	struct stat st;
	dev_t *nd;

	if (fstat(fd, &st) != 0) {
		return;
	}
	for (int i = 0; i < PHASYNC_G(npipe_nowait_off); i++) {
		if (PHASYNC_G(pipe_nowait_off)[i] == st.st_dev) {
			return;
		}
	}
	nd = realloc(PHASYNC_G(pipe_nowait_off), (PHASYNC_G(npipe_nowait_off) + 1) * sizeof(dev_t));
	if (nd) {
		PHASYNC_G(pipe_nowait_off) = nd;
		nd[PHASYNC_G(npipe_nowait_off)++] = st.st_dev;
	}
}

#endif

/* One non-blocking attempt at a pipe/TTY/FIFO read, WITHOUT ever touching
 * the fd's O_NONBLOCK flag: preadv2(..., RWF_NOWAIT) asks the kernel to fail
 * with EAGAIN instead of blocking, for exactly this one call, like
 * MSG_DONTWAIT already does for a socket's recv() just below. Falls back to
 * the old toggle-around-the-syscall approach for this fd's device if it
 * (or the running kernel) doesn't support RWF_NOWAIT at all. `flags` is the
 * fd's real, caller-visible fcntl(F_GETFL) flags (the fallback path's
 * restore value). */
static ssize_t phasync_pipe_read_nowait(php_socket_t fd, char *buf, size_t count, int flags)
{
#if PHASYNC_HAVE_NOWAIT
	if (!phasync_pipe_dev_blocked(fd)) {
		struct iovec iov = { buf, count };
		ssize_t n = preadv2(fd, &iov, 1, -1, RWF_NOWAIT);

		if (n >= 0 || (errno != ENOSYS && errno != EOPNOTSUPP && errno != EINVAL)) {
			PHASYNC_G(sw_pipe_nowait)++;
			return n;
		}
		phasync_pipe_dev_block(fd);
	}
#endif
	PHASYNC_G(sw_pipe_fcntl_fallback)++;
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	ssize_t n = read(fd, buf, count);
	fcntl(fd, F_SETFL, flags);
	return n;
}

/* Write-side twin of phasync_pipe_read_nowait(), via pwritev2(). */
static ssize_t phasync_pipe_write_nowait(php_socket_t fd, const char *buf, size_t count, int flags)
{
#if PHASYNC_HAVE_NOWAIT
	if (!phasync_pipe_dev_blocked(fd)) {
		struct iovec iov = { (void *) buf, count };
		ssize_t n = pwritev2(fd, &iov, 1, -1, RWF_NOWAIT);

		if (n >= 0 || (errno != ENOSYS && errno != EOPNOTSUPP && errno != EINVAL)) {
			PHASYNC_G(sw_pipe_nowait)++;
			return n;
		}
		phasync_pipe_dev_block(fd);
	}
#endif
	PHASYNC_G(sw_pipe_fcntl_fallback)++;
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	ssize_t n = write(fd, buf, count);
	fcntl(fd, F_SETFL, flags);
	return n;
}

/* One non-blocking send() on a blocking socket: MSG_DONTWAIT, no flag touched.
 * macOS ignores MSG_DONTWAIT in send() (unix and TCP sockets block), so there
 * O_NONBLOCK is set around the one call, as the pipe fallback above does. */
static ssize_t phasync_send_nowait(php_socket_t fd, const char *buf, size_t count, int flags)
{
#ifdef __APPLE__
	ssize_t n;
	int err;

	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	n = send(fd, buf, count, MSG_NOSIGNAL);
	err = errno;
	fcntl(fd, F_SETFL, flags);
	errno = err;
	return n;
#else
	(void) flags;
	return send(fd, buf, count, MSG_DONTWAIT | MSG_NOSIGNAL);
#endif
}

/* The wrapped read/write ops emulate the caller's intended semantics:
 *   - outside a manage() scope                  -> exactly the native op;
 *   - an explicitly non-blocking stream          -> exactly the native op;
 *   - a blocking stream inside a scope           -> cooperate: wait for
 *     readiness, then do ONE non-blocking attempt at the syscall, without ever
 *     leaving O_NONBLOCK set on the fd afterwards. A socket fd gets this via
 *     MSG_DONTWAIT on recv()/send() (no flag touched at all, since the fd is
 *     shared with every other holder of the same open file description); a
 *     pipe/TTY/FIFO fd gets this via preadv2()/pwritev2() with RWF_NOWAIT
 *     (phasync_pipe_read_nowait()/_write_nowait(), above) -- also no flag
 *     touched, on any kernel new enough to support it (Linux 4.14+); older
 *     kernels fall back to the previous approach, O_NONBLOCK set immediately
 *     before that one syscall and restored immediately after, so the window
 *     in which another process sharing the descriptor could observe it
 *     changed is just that syscall, never the wait.
 * So a wrapped stream is indistinguishable from an unwrapped one whenever no
 * scope is driving it, and never leaves the fd's mode different from how it
 * found it. */
static ssize_t phasync_wrapped_read(php_stream *stream, char *buf, size_t count)
{
	const php_stream_ops *orig = PHASYNC_ORIG(stream);
	php_socket_t fd = phasync_stream_fd(stream);
	int dir = phasync_reading();
	int flags;
	bool is_sock;

	if (fd == -1) {
		return orig->read(stream, buf, count);
	}
	flags = phasync_fd_flags(fd);
	if (flags == -1 || (flags & O_NONBLOCK) || !dir) {
		return orig->read(stream, buf, count);
	}
	is_sock = phasync_sock_data(stream) != NULL;
	double deadline = -1;              /* per op call = per fill */
	for (;;) {
		ssize_t n;
		if (is_sock) {
			n = recv(fd, buf, count, MSG_DONTWAIT);
		} else {
			n = phasync_pipe_read_nowait(fd, buf, count, flags);
		}
		if (n > 0) {
			return n;
		}
		if (n == 0) {
			stream->eof = 1;
			return 0;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			stream->eof = 1;
			return -1;
		}
		if (phasync_read_dont_wait(stream)) {
			return 0;                    /* native MSG_DONTWAIT short read */
		}
		if (phasync_cooperate(dir, stream, fd, &deadline) != PHASYNC_COOP_RETRY) {
			return -1;                   /* timed out (flag set) or exception pending */
		}
	}
}

static ssize_t phasync_wrapped_write(php_stream *stream, const char *buf, size_t count)
{
	const php_stream_ops *orig = PHASYNC_ORIG(stream);
	php_socket_t fd = phasync_stream_fd(stream);
	int dir = phasync_writing();
	int flags;
	bool is_sock;

	if (fd == -1) {
		return orig->write(stream, buf, count);
	}
	flags = phasync_fd_flags(fd);
	if (flags == -1 || (flags & O_NONBLOCK) || !dir) {
		return orig->write(stream, buf, count);
	}
	is_sock = phasync_sock_data(stream) != NULL;
	double deadline = -1;              /* per op call = per fill */
	for (;;) {
		ssize_t n;
		int err, c;
		if (is_sock) {
			n = phasync_send_nowait(fd, buf, count, flags);
		} else {
			n = phasync_pipe_write_nowait(fd, buf, count, flags);
		}
		if (n >= 0) {
			return n;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			return -1;
		}
		err = errno;
		c = phasync_cooperate(dir, stream, fd, &deadline);
		if (c == PHASYNC_COOP_RETRY) {
			continue;
		}
		if (c == PHASYNC_COOP_TIMEOUT) {
			phasync_write_timeout_notice(stream, count, err);
		}
		return -1;
	}
}

/* Delegate-mode ops for streams whose bytes must go through the original op
 * (e.g. TLS: SSL_read/SSL_write). We can't raw read/write the fd (OpenSSL's own
 * BIO does the actual recv/send), so inside a scope we tell the stream layer to
 * go non-blocking for the duration of this one op (so the original op reports
 * would-block as 0 without eof instead of blocking), wait and retry, then put it
 * back exactly as found before returning — one op call is the unit a plain
 * blocking stream_socket_enable_crypto()-driven read/write is native's
 * equivalent of one syscall, so this is the same "restore right after" rule, just
 * scoped to the whole delegate call instead of one recv/send. Read/write intent
 * is approximated (a read waits for readability); TLS renegotiation wanting the
 * opposite direction is a known limitation. */
/* A TLS stream's socket, without php_stream_cast(): openssl's cast for select()
 * fills the read buffer when OpenSSL holds decrypted data (SSL_pending()) and
 * the buffer is empty, through this stream's read op, which asked for the fd:
 * an endless recursion. Its stream data starts with a php_netstream_data_t. */
static zend_always_inline php_socket_t phasync_tls_fd(php_stream *stream)
{
	return ((php_netstream_data_t *) stream->abstract)->socket;
}

static ssize_t phasync_wrapped_read_tls(php_stream *stream, char *buf, size_t count)
{
	const php_stream_ops *orig = PHASYNC_ORIG(stream);
	php_socket_t fd = phasync_tls_fd(stream);
	int dir = phasync_reading();
	int flags = phasync_fd_flags(fd);
	ssize_t n;

	if (flags == -1 || (flags & O_NONBLOCK) || !dir) {
		return orig->read(stream, buf, count);
	}
	double deadline = -1;              /* per op call = per fill */
	if (orig->set_option) {
		orig->set_option(stream, PHP_STREAM_OPTION_BLOCKING, 0, NULL);
	}
	for (;;) {
		n = orig->read(stream, buf, count);
		if (n > 0 || n < 0 || stream->eof) {
			break;                        /* data, error, or real EOF */
		}
		fd = phasync_tls_fd(stream);  /* n == 0, not eof -> would block */
		if (fd == -1 || phasync_cooperate(dir, stream, fd, &deadline) != PHASYNC_COOP_RETRY) {
			n = -1;
			break;
		}
	}
	if (orig->set_option) {
		orig->set_option(stream, PHP_STREAM_OPTION_BLOCKING, 1, NULL);
	}
	return n;
}

static ssize_t phasync_wrapped_write_tls(php_stream *stream, const char *buf, size_t count)
{
	const php_stream_ops *orig = PHASYNC_ORIG(stream);
	php_socket_t fd = phasync_tls_fd(stream);
	int dir = phasync_writing();
	int flags = phasync_fd_flags(fd);
	ssize_t n;

	if (flags == -1 || (flags & O_NONBLOCK) || !dir) {
		return orig->write(stream, buf, count);
	}
	double deadline = -1;              /* per op call = per fill */
	if (orig->set_option) {
		orig->set_option(stream, PHP_STREAM_OPTION_BLOCKING, 0, NULL);
	}
	for (;;) {
		n = orig->write(stream, buf, count);
		if (n > 0 || n < 0) {
			break;
		}
		fd = phasync_tls_fd(stream);  /* 0 -> would block */
		if (fd == -1 || phasync_cooperate(dir, stream, fd, &deadline) != PHASYNC_COOP_RETRY) {
			n = -1;
			break;
		}
	}
	if (orig->set_option) {
		orig->set_option(stream, PHP_STREAM_OPTION_BLOCKING, 1, NULL);
	}
	return n;
}

/* Delegate a set_option call to the original ops. xport ops (bind/connect/listen/
 * accept/getname/…) all arrive through set_option, and the socket layer decides
 * unix-vs-inet by *pointer identity* (php_stream_is(stream,
 * &php_stream_unix_socket_ops), xp_socket.c). Our wrapped ops is a copy with a
 * different address, which would make a unix socket look like inet ("Failed to
 * parse address"), so restore the real ops pointer for the (synchronous) call. */
static int phasync_orig_set_option(php_stream *stream, int option, int value, void *ptrparam)
{
	const php_stream_ops *orig = PHASYNC_ORIG(stream), *saved = stream->ops;
	int r;

	if (!orig->set_option) {
		return PHP_STREAM_OPTION_RETURN_NOTIMPL;
	}
	if (saved->read == phasync_wrapped_read_tls) {
		/* ext/openssl never compares ops pointers, and its connect/accept enable
		 * crypto through stream->ops: keep ours there, so the handshake cooperates
		 * and accepted clients inherit the wrapped ops. */
		return orig->set_option(stream, option, value, ptrparam);
	}
	stream->ops = (php_stream_ops *) orig;
	r = orig->set_option(stream, option, value, ptrparam);
	stream->ops = saved;
	return r;
}

/* stream_socket_accept() with a timeout, inside a scope and in a fiber: wait for
 * the listener to become readable via the read handler instead of blocking the
 * process, then accept with a zero timeout. On timeout, report exactly what the
 * socket layer reports (php_network_accept_incoming: ETIMEDOUT + its error text),
 * so the calling PHP function emits its own native warning. A zero timeout keeps
 * its explicit don't-wait meaning and goes straight to the original. */
static int phasync_accept_cooperative(php_stream *stream, php_stream_xport_param *xp)
{
	struct timeval *tv = xp->inputs.timeout, zero = { 0, 0 };
	double deadline = tv ? phasync_now() + (double) tv->tv_sec + (double) tv->tv_usec / 1000000.0 : 0;
	php_socket_t fd = phasync_stream_fd(stream);

	if (fd == -1 || (tv && tv->tv_sec == 0 && tv->tv_usec == 0)) {
		return phasync_orig_set_option(stream, PHP_STREAM_OPTION_XPORT_API, 0, xp);
	}
	for (;;) {
		double remaining = tv ? deadline - phasync_now() : INFINITY;
		int w = remaining > 0
			? phasync_wait_fd(PHASYNC_READ, stream, fd, remaining)
			: PHASYNC_WAIT_TIMEOUT;
		int r;

		if (w == PHASYNC_WAIT_TIMEOUT) {
			xp->outputs.client = NULL;
			xp->outputs.returncode = -1;
			xp->outputs.error_code = ETIMEDOUT;
			if (xp->want_errortext) {
				xp->outputs.error_text = php_socket_error_str(ETIMEDOUT);
			}
			return PHP_STREAM_OPTION_RETURN_OK;
		}
		if (w == PHASYNC_WAIT_ERROR || w == PHASYNC_WAIT_CLOSED) {
			/* Closed meanwhile (EBADF), or the wait cancelled or its coroutine
			 * destroyed (ECANCELED, the exception pending propagates). */
			xp->outputs.client = NULL;
			xp->outputs.returncode = -1;
			xp->outputs.error_code = w == PHASYNC_WAIT_CLOSED ? EBADF : ECANCELED;
			if (xp->want_errortext) {
				xp->outputs.error_text = php_socket_error_str(xp->outputs.error_code);
			}
			return PHP_STREAM_OPTION_RETURN_OK;
		}
		xp->inputs.timeout = &zero;
		r = phasync_orig_set_option(stream, PHP_STREAM_OPTION_XPORT_API, 0, xp);
		xp->inputs.timeout = tv;
		if (r != PHP_STREAM_OPTION_RETURN_OK || xp->outputs.client
		 || (xp->outputs.error_code != ETIMEDOUT && xp->outputs.error_code != EAGAIN
		     && xp->outputs.error_code != EWOULDBLOCK)) {
			return r;
		}
		/* Someone else took the connection between readiness and accept: wait again. */
		if (xp->outputs.error_text) {
			zend_string_release(xp->outputs.error_text);
			xp->outputs.error_text = NULL;
		}
	}
}

/* tcp:// connect (stream_socket_client/fsockopen/pfsockopen), inside a scope and
 * in a fiber. Native resolves the host (blocking getaddrinfo), then tries each
 * address in order within one timeout budget, each attempt a blocking connect.
 * Here: resolve on the worker pool (same hints as php_network_getaddresses, so
 * the same order), then for each address hand the ORIGINAL connect that single IP
 * literal in async mode — PHP still creates the socket with every context option
 * (bindto, tcp_nodelay, keepalive, linger, buffers) — and wait for writability via
 * the write handler with the remaining budget. Errors are recorded exactly as the
 * socket layer does, so the calling PHP function emits its own native warning. A
 * failed lookup falls back to the native connect so its error message is PHP's. */
static int phasync_ipv6_usable(void)
{
	static int usable = -1;       /* php_network_getaddresses' ipv6_borked probe */
	if (usable == -1) {
		int s6 = socket(AF_INET6, SOCK_DGRAM, 0);
		usable = s6 >= 0;
		if (s6 >= 0) {
			close(s6);
		}
	}
	return usable;
}

static int phasync_connect_cooperative(php_stream *stream, php_stream_xport_param *xp)
{
	php_netstream_data_t *sock = phasync_sock_data(stream);
	char *name = xp->inputs.name, host[256], literal[INET6_ADDRSTRLEN + 16];
	size_t namelen = xp->inputs.namelen, hostlen, portlen;
	const char *port;
	struct timeval *tv = xp->inputs.timeout;
	double deadline;
	struct addrinfo hints, *res = NULL, *ai;
	unsigned char bin[sizeof(struct in6_addr)];
	int attempted = 0;

	if (sock == NULL || name == NULL || namelen == 0) {
		return phasync_orig_set_option(stream, PHP_STREAM_OPTION_XPORT_API, 0, xp);
	}
	/* "host:port" or "[v6]:port", as xp_socket's parse_ip_address reads it */
	if (name[0] == '[') {
		const char *end = memchr(name, ']', namelen);
		if (!end || end + 1 >= name + namelen || end[1] != ':') {
			return phasync_orig_set_option(stream, PHP_STREAM_OPTION_XPORT_API, 0, xp);
		}
		hostlen = end - name - 1;
		memcpy(host, name + 1, MIN(hostlen, sizeof(host) - 1));
		port = end + 2;
	} else {
		const char *colon = zend_memrchr(name, ':', namelen);
		if (!colon) {
			return phasync_orig_set_option(stream, PHP_STREAM_OPTION_XPORT_API, 0, xp);
		}
		hostlen = colon - name;
		memcpy(host, name, MIN(hostlen, sizeof(host) - 1));
		port = colon + 1;
	}
	if (hostlen >= sizeof(host)) {
		return phasync_orig_set_option(stream, PHP_STREAM_OPTION_XPORT_API, 0, xp);
	}
	host[hostlen] = '\0';
	portlen = name + namelen - port;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = phasync_ipv6_usable() ? AF_UNSPEC : AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (inet_pton(AF_INET, host, bin) == 1 || inet_pton(AF_INET6, host, bin) == 1) {
		hints.ai_flags = AI_NUMERICHOST;             /* no DNS to offload */
		if (getaddrinfo(host, NULL, &hints, &res) != 0) {
			res = NULL;
		}
	} else {
		phasync_task t;
		memset(&t, 0, sizeof(t));
		t.type  = PHASYNC_OP_GETADDRINFO;
		t.host  = host;
		t.hints = hints;
		phasync_pool_run(&t);
		if (EG(exception)) {
			if (t.ai) freeaddrinfo(t.ai);
			xp->outputs.returncode = -1;
			return PHP_STREAM_OPTION_RETURN_OK;       /* exception pending: propagates */
		}
		res = t.result == 0 ? t.ai : NULL;
		if (!res && t.ai) freeaddrinfo(t.ai);
	}
	if (res == NULL) {
		/* Lookup failed: let the native connect produce PHP's exact error. */
		return phasync_orig_set_option(stream, PHP_STREAM_OPTION_XPORT_API, 0, xp);
	}
	/* Like native, the connect budget starts after the lookup, not before it. */
	deadline = tv ? phasync_now() + (double) tv->tv_sec + (double) tv->tv_usec / 1000000.0 : 0;

	for (ai = res; ai; ai = ai->ai_next) {
		double remaining = tv ? deadline - phasync_now() : INFINITY;
		char ip[INET6_ADDRSTRLEN];
		int r;

		if (attempted && remaining <= 0) {
			break;                    /* native: no further attempts once time is up */
		}
		if (getnameinfo(ai->ai_addr, ai->ai_addrlen, ip, sizeof(ip), NULL, 0, NI_NUMERICHOST) != 0) {
			continue;
		}
		snprintf(literal, sizeof(literal), ai->ai_family == AF_INET6 ? "[%s]:%.*s" : "%s:%.*s",
			ip, (int) portlen, port);
		if (xp->outputs.error_text) {  /* native frees the previous attempt's error */
			zend_string_release(xp->outputs.error_text);
			xp->outputs.error_text = NULL;
		}
		xp->inputs.name = literal;
		xp->inputs.namelen = strlen(literal);
		xp->op = STREAM_XPORT_OP_CONNECT_ASYNC;
		r = phasync_orig_set_option(stream, PHP_STREAM_OPTION_XPORT_API, 0, xp);
		xp->op = STREAM_XPORT_OP_CONNECT;
		xp->inputs.name = name;
		xp->inputs.namelen = namelen;
		attempted = 1;
		if (r != PHP_STREAM_OPTION_RETURN_OK) {
			freeaddrinfo(res);
			return r;
		}
		if (xp->outputs.returncode == 1) {                 /* EINPROGRESS */
			int err = 0;
			socklen_t l = sizeof(err);
			/* Wait on a transient resource we own, never the stream's own: the stream
			 * is still being created, and its creator may not expect anyone else to
			 * hold it — mysqlnd frees its stream's zend_resource raw right after
			 * connecting (mysqlnd_fixup_regular_list), which would leave a handler's
			 * references dangling (heap corruption). */
			int w = remaining > 0
				? phasync_wait_fd(PHASYNC_WRITE, NULL, sock->socket, remaining)
				: PHASYNC_WAIT_TIMEOUT;
			if (w == PHASYNC_WAIT_ERROR) {
				close(sock->socket);
				sock->socket = -1;
				xp->outputs.returncode = -1;
				freeaddrinfo(res);
				return PHP_STREAM_OPTION_RETURN_OK;       /* exception pending: propagates */
			}
			if (w == PHASYNC_WAIT_TIMEOUT) {
				err = ETIMEDOUT;
			} else if (getsockopt(sock->socket, SOL_SOCKET, SO_ERROR, &err, &l) != 0) {
				err = errno;
			}
			if (err == 0) {
				xp->outputs.returncode = 0;
			} else {
				close(sock->socket);
				sock->socket = -1;
				xp->outputs.returncode = -1;
				xp->outputs.error_code = err;
				if (xp->want_errortext) {
					xp->outputs.error_text = php_socket_error_str(err);
				}
				if (w == PHASYNC_WAIT_TIMEOUT) {
					break;
				}
				continue;
			}
		}
		if (xp->outputs.returncode == 0) {
			/* Connected. The native sync path leaves the socket blocking. */
			int fl = fcntl(sock->socket, F_GETFL, 0);
			if (fl != -1) {
				fcntl(sock->socket, F_SETFL, fl & ~O_NONBLOCK);
			}
			xp->outputs.error_code = 0;
			if (xp->outputs.error_text) {
				zend_string_release(xp->outputs.error_text);
				xp->outputs.error_text = NULL;
			}
			freeaddrinfo(res);
			return PHP_STREAM_OPTION_RETURN_OK;
		}
		/* -1: failed at once; the socket layer closed it and recorded the error */
	}
	freeaddrinfo(res);
	xp->outputs.returncode = -1;
	return PHP_STREAM_OPTION_RETURN_OK;
}

/* Record the caller's intended blocking mode and pass it through, so the native
 * op (used outside a scope) sees the real intent while we still know it. */
/* stream_socket_recvfrom()/sendto() go through the transport (recvfrom()/
 * sendto() on the socket), not the read/write ops. Inside a scope, in a fiber,
 * on a blocking stream: wait until the socket is ready, then let the original
 * do the call in blocking mode, which then doesn't wait, so results and
 * warnings stay native. PHP applies no timeout here, so neither does the wait.
 * Out-of-band data goes straight to the original. */
static int phasync_xport_io_cooperative(php_stream *stream, php_stream_xport_param *xp, int option, int value)
{
	php_socket_t fd = phasync_stream_fd(stream);
	int flags = phasync_fd_flags(fd);
	int dir = xp->op == STREAM_XPORT_OP_RECV ? PHASYNC_READ : PHASYNC_WRITE;

	if (flags != -1 && !(flags & O_NONBLOCK) && !(xp->inputs.flags & STREAM_OOB)) {
		struct pollfd pfd = { fd, dir == PHASYNC_READ ? POLLIN : POLLOUT, 0 };
		while (poll(&pfd, 1, 0) == 0) {
			if (phasync_wait_fd(dir, stream, fd, INFINITY) != PHASYNC_WAIT_READY) {
				return PHP_STREAM_OPTION_RETURN_ERR;   /* exception pending */
			}
		}
		/* fd is still blocking (we never touched it): the original's own
		 * recvfrom()/sendto() call below won't actually block now that we know
		 * it's ready. */
	}
	return phasync_orig_set_option(stream, option, value, xp);
}

static php_stream_ops *phasync_wrapped_ops_for(const php_stream_ops *orig, phasync_mode mode);

/* Enabling crypto (stream_socket_enable_crypto(), and the handshake in tls://
 * connect and accept) on a blocking stream loops on the handshake, polling the
 * socket in between. Inside a scope, in a fiber: run it non-blocking, where each
 * call makes one attempt and returns 0 while it would block, and park between
 * attempts, as native PHP polls (no timeout, as natively). Which way to wait is
 * not exposed; a handshake that wants to write finds the socket not writable, so
 * wait for writable then, else readable. Afterwards the stream's reads and writes
 * must go through the original (SSL_read/SSL_write), or come off it again. */
static int phasync_crypto_enable(php_stream *stream, int option, int value, php_stream_xport_crypto_param *cp)
{
	const php_stream_ops *orig = PHASYNC_ORIG(stream);
	php_socket_t fd = phasync_stream_fd(stream);
	int flags = phasync_fd_flags(fd);
	bool is_blocking = flags != -1 && !(flags & O_NONBLOCK);
	int rc;

	if (cp->inputs.activate && is_blocking && phasync_reading() && EG(active_fiber) != NULL) {
		orig->set_option(stream, PHP_STREAM_OPTION_BLOCKING, 0, NULL);
		for (;;) {
			struct pollfd pfd = { fd, POLLOUT, 0 };
			rc = orig->set_option(stream, option, value, cp);
			if (rc != PHP_STREAM_OPTION_RETURN_OK || cp->outputs.returncode != 0) {
				break;
			}
			if (phasync_wait_fd(poll(&pfd, 1, 0) == 0 ? PHASYNC_WRITE : PHASYNC_READ, stream, fd, INFINITY)
			    != PHASYNC_WAIT_READY) {
				cp->outputs.returncode = -1;     /* exception pending */
				break;
			}
		}
		orig->set_option(stream, PHP_STREAM_OPTION_BLOCKING, 1, NULL);
	} else {
		rc = orig->set_option(stream, option, value, cp);
	}
	if (rc == PHP_STREAM_OPTION_RETURN_OK && cp->outputs.returncode == 1 && cp->inputs.activate
	 && stream->ops->read == phasync_wrapped_read) {
		stream->ops = phasync_wrapped_ops_for(orig, PHASYNC_MODE_TLS);
	} else if (!cp->inputs.activate && stream->ops->read == phasync_wrapped_read_tls) {
		/* crypto is off after a disable (which returns -1 even when it succeeds) */
		stream->ops = phasync_wrapped_ops_for(orig, PHASYNC_MODE_RAW);
	}
	return rc;
}

static void phasync_wrap_stream(php_stream *stream, phasync_mode mode);
static int phasync_wrapped_set_option_inner(php_stream *stream, int option, int value, void *ptrparam);

/* An accepted client is created with the listener's ops at the time, which are
 * the original ones while the socket layer runs (phasync_orig_set_option()):
 * give it the listener's wrapper, so it cooperates and its close op deregisters it. */
static int phasync_wrapped_set_option(php_stream *stream, int option, int value, void *ptrparam)
{
	int rc = phasync_wrapped_set_option_inner(stream, option, value, ptrparam);

	if (option == PHP_STREAM_OPTION_XPORT_API && ptrparam
	 && ((php_stream_xport_param *) ptrparam)->op == STREAM_XPORT_OP_ACCEPT
	 && ((php_stream_xport_param *) ptrparam)->outputs.client) {
		phasync_wrap_stream(((php_stream_xport_param *) ptrparam)->outputs.client,
			stream->ops->read == phasync_wrapped_read_tls ? PHASYNC_MODE_TLS : PHASYNC_MODE_RAW);
	}
	return rc;
}

static int phasync_wrapped_set_option_inner(php_stream *stream, int option, int value, void *ptrparam)
{
	if (option == PHP_STREAM_OPTION_CRYPTO_API && ptrparam
	        && ((php_stream_xport_crypto_param *) ptrparam)->op == STREAM_XPORT_CRYPTO_OP_ENABLE) {
		return phasync_crypto_enable(stream, option, value, (php_stream_xport_crypto_param *) ptrparam);
	} else if (option == PHP_STREAM_OPTION_MMAP_API && value == PHP_STREAM_MMAP_SUPPORTED
	        && stream->ops->read == phasync_wrapped_read_pool
	        && phasync_reading() && EG(active_fiber) != NULL) {
		/* mmap()ed reads (readfile(), fpassthru(), stream_copy_to_stream()) fault
		 * pages in synchronously: report mmap unsupported so they use the pooled
		 * read op instead. */
		return PHP_STREAM_OPTION_RETURN_NOTIMPL;
	} else if (option == PHP_STREAM_OPTION_XPORT_API && ptrparam
	        && ((php_stream_xport_param *) ptrparam)->op == STREAM_XPORT_OP_ACCEPT
	        && phasync_reading() && EG(active_fiber) != NULL) {
		return phasync_accept_cooperative(stream, (php_stream_xport_param *) ptrparam);
	} else if (option == PHP_STREAM_OPTION_XPORT_API && ptrparam
	        && (((php_stream_xport_param *) ptrparam)->op == STREAM_XPORT_OP_RECV
	         || ((php_stream_xport_param *) ptrparam)->op == STREAM_XPORT_OP_SEND)
	        && stream->ops->read == phasync_wrapped_read
	        && phasync_reading() && EG(active_fiber) != NULL) {
		return phasync_xport_io_cooperative(stream, (php_stream_xport_param *) ptrparam, option, value);
	} else if (option == PHP_STREAM_OPTION_XPORT_API && ptrparam
	        && ((php_stream_xport_param *) ptrparam)->op == STREAM_XPORT_OP_CONNECT
	        && stream->ops->read == phasync_wrapped_read       /* not tls://: crypto follows */
	        && phasync_writing() && EG(active_fiber) != NULL
	        && (strcmp(PHASYNC_ORIG(stream)->label, "tcp_socket") == 0
	            || strcmp(PHASYNC_ORIG(stream)->label, "tcp_socket/ssl") == 0)) {
		return phasync_connect_cooperative(stream, (php_stream_xport_param *) ptrparam);
	}
	return phasync_orig_set_option(stream, option, value, ptrparam);
}

static int phasync_wrapped_close(php_stream *stream, int close_handle)
{
	const php_stream_ops *orig = PHASYNC_ORIG(stream);
	phasync_hook_entry *e;

	phasync_inflight_close(stream);
	e = phasync_entry(stream);

	if (e) {
		if (e->pidfd != -1) {
			/* A process pipe: pclose() below waits for the child. Inside a scope,
			 * in a fiber, wait for its exit here instead, on the pidfd (readable
			 * once the child exits, without reaping it), so pclose() then reaps at
			 * once with the native status. Like pclose(), first close our end, by
			 * duping /dev/null over it (the fd stays valid for fclose()), so a
			 * child still using the pipe gets EOF/EPIPE instead of blocking. */
			int dir = phasync_reading();
			php_socket_t fd = phasync_stream_fd(stream);
			int devnull;
			if (close_handle && dir && EG(active_fiber) && fd != -1
			 && (devnull = open("/dev/null", O_RDWR | O_CLOEXEC)) >= 0) {
				if (dup2(devnull, fd) >= 0) {
					phasync_wait_fd(dir, NULL, e->pidfd, INFINITY);
				}
				close(devnull);
			}
			close(e->pidfd);
		}
		/* Nothing to hand back here: we never leave a stream's fd mode changed
		 * after an op, so there is nothing to restore on close. */
		zend_hash_index_del(&PHASYNC_G(hooked), (zend_ulong) (uintptr_t) stream);
	}
	return orig->close(stream, close_handle);
}

/* Build (or fetch cached) a wrapped ops for the given original ops: a copy with
 * read/write/close overridden and every other op left as the original's. */
static php_stream_ops *phasync_wrapped_ops_for(const php_stream_ops *orig, phasync_mode mode)
{
	/* One orig ops (e.g. stdio) can be wrapped in different modes (a regular file
	 * as POOL, a FIFO as RAW), so the cache key folds the mode into the low bits
	 * of the (pointer-aligned) orig address. */
	zend_ulong key = ((zend_ulong) (uintptr_t) orig) | (zend_ulong) mode;
	phasync_wops *w = zend_hash_index_find_ptr(&PHASYNC_G(wrapped_ops_cache), key);
	if (w) {
		return &w->ops;
	}
	w = pemalloc(sizeof(*w), 1);
	w->ops = *orig;   /* label too: it is public (stream_type) and mysqlnd checks it (issue #63) */
	switch (mode) {
		case PHASYNC_MODE_TLS:
			w->ops.read  = phasync_wrapped_read_tls;
			w->ops.write = phasync_wrapped_write_tls;
			break;
		case PHASYNC_MODE_POOL:
			w->ops.read  = phasync_wrapped_read_pool;
			w->ops.write = phasync_wrapped_write_pool;
			break;
		default:
			w->ops.read  = phasync_wrapped_read;
			w->ops.write = phasync_wrapped_write;
			break;
	}
	w->ops.close = phasync_wrapped_close;
	w->ops.set_option = phasync_wrapped_set_option;
	w->orig = orig;
	zend_hash_index_add_ptr(&PHASYNC_G(wrapped_ops_cache), key, w);
	return &w->ops;
}

/* Wrap any descriptor-backed stream (socket or pipe). Ops replacement only;
 * fd/non-blocking setup is deferred to first I/O. */
static void phasync_wrap_stream(php_stream *stream, phasync_mode mode)
{
	if (stream == NULL || stream->ops == NULL) {
		return;
	}
	/* Already wrapped? (also covers accepted sockets that inherited wrapped ops) */
	if (stream->ops->read == phasync_wrapped_read
	 || stream->ops->read == phasync_wrapped_read_tls
	 || stream->ops->read == phasync_wrapped_read_pool) {
		return;
	}
	stream->ops = phasync_wrapped_ops_for(stream->ops, mode);
}

/* ---- transport factories ------------------------------------------------- */

static php_stream *phasync_tcp_factory(const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen, const char *persistent_id,
		int options, int flags, struct timeval *timeout,
		php_stream_context *context STREAMS_DC)
{
	php_stream *s = PHASYNC_G(orig_tcp)(proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_CC);
	phasync_wrap_stream(s, PHASYNC_MODE_RAW);
	return s;
}

static php_stream *phasync_udp_factory(const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen, const char *persistent_id,
		int options, int flags, struct timeval *timeout,
		php_stream_context *context STREAMS_DC)
{
	php_stream *s = PHASYNC_G(orig_udp)(proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_CC);
	phasync_wrap_stream(s, PHASYNC_MODE_RAW);
	return s;
}

static php_stream *phasync_udg_factory(const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen, const char *persistent_id,
		int options, int flags, struct timeval *timeout,
		php_stream_context *context STREAMS_DC)
{
	php_stream *s = PHASYNC_G(orig_udg)(proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_CC);
	phasync_wrap_stream(s, PHASYNC_MODE_RAW);
	return s;
}

static php_stream *phasync_unix_factory(const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen, const char *persistent_id,
		int options, int flags, struct timeval *timeout,
		php_stream_context *context STREAMS_DC)
{
	php_stream *s = PHASYNC_G(orig_unix)(proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_CC);
	phasync_wrap_stream(s, PHASYNC_MODE_RAW);
	return s;
}

static php_stream *phasync_ssl_factory(const char *proto, size_t protolen,
		const char *resourcename, size_t resourcenamelen, const char *persistent_id,
		int options, int flags, struct timeval *timeout,
		php_stream_context *context STREAMS_DC)
{
	php_stream *s = PHASYNC_G(orig_ssl)(proto, protolen, resourcename, resourcenamelen,
		persistent_id, options, flags, timeout, context STREAMS_CC);
	phasync_wrap_stream(s, PHASYNC_MODE_TLS);   /* TLS: delegate to SSL_read/SSL_write */
	return s;
}

/* The openssl extension registers one factory under several scheme names. */
static const char *phasync_ssl_schemes[] = {
	"ssl", "tls", "sslv3", "tlsv1.0", "tlsv1.1", "tlsv1.2", "tlsv1.3", NULL
};

static int phasync_fs_path(zval *z, char *out);
static bool phasync_fs_fd_offloaded(php_socket_t fd);
static int phasync_fs_fd_mode(php_socket_t fd);

/* ---- stream_select() / socket_select() inside a scope --------------------
 *
 * A library's own stream_select()/socket_select() with a timeout would block the
 * whole process. Inside a scope, in a fiber, the call instead:
 *   1. probes the ORIGINAL function with a zero timeout, so every result (arrays,
 *      count, buffered-data emulation, warnings) is exactly native;
 *   2. if nothing is ready, registers all the descriptors in an epoll instance —
 *      itself a pollable fd, readable when any of them is — and waits on it via
 *      the read handler with the remaining timeout;
 *   3. probes again; a handler timeout means the select timed out (native: 0, all
 *      arrays emptied). */

/* *stream_out is this element's close-tracking key: the php_stream* for
 * stream_select() (matching phasync_stream_forget()), always NULL for
 * socket_select() (matching phasync_socket_close_override()'s bare-fd
 * registrations) -- see phasync_select_watch_add(). */
static php_socket_t phasync_select_fd_stream(zval *elem, php_stream **stream_out)
{
	php_stream *s = NULL;
	php_socket_t fd = -1;

	ZVAL_DEREF(elem);
	if (Z_TYPE_P(elem) != IS_RESOURCE) {
		return -1;
	}
	php_stream_from_zval_no_verify(s, elem);
	if (s == NULL || php_stream_cast(s, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL,
			(void *) &fd, 0) != SUCCESS) {
		return -1;
	}
	*stream_out = s;
	return fd;
}

/* ext/sockets' php_socket, mirrored here so socket_select() support doesn't depend
 * on its header, which PHP builds (e.g. the official Docker images) often don't
 * install. The layout is identical from PHP 8.2 to master. */
typedef struct {
	int         bsd_socket;
	int         type;
	int         error;
	int         blocking;
	zval        zstream;
	zend_object std;
} phasync_php_socket;

static php_socket_t phasync_select_fd_socket(zval *elem)
{
	struct stat st;
	int fd;

	ZVAL_DEREF(elem);
	/* Matched by name rather than socket_ce, so ext/sockets stays optional. */
	if (Z_TYPE_P(elem) != IS_OBJECT || !zend_string_equals_literal(Z_OBJCE_P(elem)->name, "Socket")) {
		return -1;
	}
	fd = ((phasync_php_socket *) ((char *) Z_OBJ_P(elem) - offsetof(phasync_php_socket, std)))->bsd_socket;
	/* Safety net should a future layout differ: only ever use a real socket fd. */
	return (fd >= 0 && fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode)) ? fd : -1;
}

/* phasync_select_fd_socket(), plus the close-tracking key (always NULL: a
 * socket is never a php_stream, see phasync_select_fd_stream()). */
static php_socket_t phasync_select_fd_socket_ex(zval *elem, php_stream **stream_out)
{
	php_socket_t fd = phasync_select_fd_socket(elem);
	if (fd >= 0) {
		*stream_out = NULL;
	}
	return fd;
}

/* An epoll fd watching every descriptor in the three sets (read/write/except),
 * or -1 if none could be registered. */
static int phasync_select_epoll(zval *sets[3], php_socket_t (*fd_of)(zval *, php_stream **))
{
	static const uint32_t ev[3] = { PHASYNC_EV_IN, PHASYNC_EV_OUT, PHASYNC_EV_PRI };
	HashTable events;
	zval *elem;
	int epfd, i, n = 0;

	zend_hash_init(&events, 8, NULL, NULL, 0);
	for (i = 0; i < 3; i++) {
		if (Z_TYPE_P(sets[i]) != IS_ARRAY) {
			continue;
		}
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(sets[i]), elem) {
			php_stream *s;
			php_socket_t fd = fd_of(elem, &s);
			zval *cur;
			if (fd < 0) {
				continue;
			}
			if ((cur = zend_hash_index_find(&events, (zend_ulong) fd))) {
				Z_LVAL_P(cur) |= ev[i];
			} else {
				zval z;
				ZVAL_LONG(&z, ev[i]);
				zend_hash_index_add_new(&events, (zend_ulong) fd, &z);
			}
		} ZEND_HASH_FOREACH_END();
	}
	epfd = phasync_mux_open();
	if (epfd >= 0) {
		zend_ulong fd;
		zval *e;
		ZEND_HASH_FOREACH_NUM_KEY_VAL(&events, fd, e) {
			if (phasync_mux_add(epfd, (int) fd, (uint32_t) Z_LVAL_P(e)) == 0) {
				n++;          /* regular files can't be registered (EPERM): always ready */
			}
		} ZEND_HASH_FOREACH_END();
		if (n == 0) {
			close(epfd);
			epfd = -1;
		}
	}
	zend_hash_destroy(&events);
	return epfd;
}

/* ---- stream_select()/socket_select(): waking on a close mid-wait (#16) ----
 *
 * phasync_select_epoll()'s epfd is private to this call and unknown to any
 * Poller, so a watched stream's own descriptor closing -- by another
 * coroutine, while we wait -- never reaches it: Linux drops a closed fd from
 * epoll silently (there is no "the peer closed its end" event when OUR OWN
 * process is the one that closed it), so without more the wait only ever
 * ends at its timeout (or never, with none).
 *
 * A close already wakes an ordinary read/write wait on that stream, through
 * its Poller registration (phasync_stream_forget() for a php_stream,
 * phasync_socket_close_override() for a bare socket fd). select() gets the
 * same wake by registering each watched stream there too -- one phasync_reg
 * slot per stream, under the SAME slot number as the epfd wait, so a close
 * pushes it exactly like a ready epfd would; phasync_park() only ever needs
 * one slot to return. Best-effort: a stream already claimed in both
 * directions by another coroutine's real, concurrent wait keeps today's
 * behavior for that one stream (it is not watched for a fast close; the call
 * still wakes, natively, at the timeout). */
typedef struct {
	php_socket_t fd;
	php_stream  *stream;   /* the close-tracking key: NULL for a socket */
	int          idx;      /* the direction slot claimed: 0 read, 1 write */
} phasync_select_watch;

/* Claim close-notice on (stream, fd) under slot, in whichever direction is
 * free. False (nothing claimed: both directions already belong to a real,
 * concurrent waiter) leaves *w untouched. */
static bool phasync_select_watch_add(phasync_poller *p, php_stream *stream, php_socket_t fd,
                                     zend_long slot, phasync_select_watch *w)
{
	phasync_reg *r = zend_hash_index_find_ptr(&p->regs, (zend_ulong) fd);

	if (r != NULL && r->stream != stream) {
		return false;   /* owned by a different, still-live registration */
	}
	if (r == NULL) {
		r = pecalloc(1, sizeof(*r), 1);
		r->fd = fd;
		r->stream = stream;
		r->slot[0] = r->slot[1] = -1;
		r->sid[0] = r->sid[1] = -1;
		r->hooked = stream != NULL && phasync_has_close_hook(stream);
		zend_hash_index_add_new_ptr(&p->regs, (zend_ulong) fd, r);
	}
	if (r->slot[0] >= 0 && r->slot[1] >= 0) {
		return false;   /* both directions already belong to a real waiter */
	}
	w->idx = r->slot[0] < 0 ? 0 : 1;
	r->slot[w->idx] = slot;
	r->watch |= 1 << w->idx;
	w->fd = fd;
	w->stream = stream;
	return true;
}

/* True once phasync_stream_forget()/phasync_socket_close_override() has found
 * and dropped this registration: a close of its stream. Never dereferences
 * the stream itself -- by the time this runs, w->stream may already be
 * freed; it is only ever used as a hash key. */
static bool phasync_select_watch_closed(phasync_poller *p, const phasync_select_watch *w)
{
	phasync_reg *r = zend_hash_index_find_ptr(&p->regs, (zend_ulong) w->fd);
	return r == NULL || r->stream != w->stream;
}

/* Not closed: let go of our slot claim, leaving the registration (it may be a
 * hooked stream's, reused by its next real op) exactly as an ordinary wait's
 * phasync_wait_settle() would. */
static void phasync_select_watch_release(phasync_poller *p, const phasync_select_watch *w)
{
	phasync_reg *r = zend_hash_index_find_ptr(&p->regs, (zend_ulong) w->fd);
	if (r != NULL && r->stream == w->stream) {
		r->slot[w->idx] = -1;
		r->watch &= ~(1 << w->idx);
	}
}

/* One descriptor named in the select's sets, found while phasync_select_epoll()'s
 * caller made its own pass (so fd_of() is never called again once we might be
 * waiting: safe against a watched stream closing under us mid-wait, and cheaper
 * than a second pass). elem points into saved[] (our own copy of the caller's
 * arrays), stable and safe to copy from regardless of what its resource does
 * meanwhile. */
typedef struct {
	php_socket_t fd;
	php_stream  *stream;
	int          set;      /* 0 read, 1 write, 2 except: which of saved[] */
	zval        *elem;
	zend_string *key_str;  /* this element's original array key: */
	zend_ulong   key_idx;  /* key_str, else key_idx (key_str == NULL) */
} phasync_select_cand;

/* Wait for the select's private epfd to be readable, or for a close of one of
 * the candidates -- whichever comes first. PHASYNC_WAIT_READY: the epfd is
 * readable, probe the sets again as usual. PHASYNC_WAIT_CLOSED: *closed is the
 * index (into cands) of one candidate that closed; the caller reports every
 * candidate sharing its fd directly, as native PHP would once the caller's own
 * next op on the same descriptor fails. PHASYNC_WAIT_TIMEOUT/_ERROR as
 * phasync_wait_fd(). */
static int phasync_select_wait(phasync_poller *p, int epfd, phasync_select_cand *cands, int ncands,
                               double timeout, zend_class_entry *timeout_ce, int *closed)
{
	phasync_select_watch *watches = ncands ? safe_emalloc(ncands, sizeof(*watches), 0) : NULL;
	int *widx = ncands ? safe_emalloc(ncands, sizeof(*widx), 0) : NULL;   /* cands[i] -> watches[], or -1 */
	int i, n = 0;
	zend_long slot;
	int rc = PHASYNC_WAIT_READY;
	phasync_reg *er;

	if ((slot = phasync_get_slot(p)) < 0) {
		if (watches) { efree(watches); efree(widx); }
		return PHASYNC_WAIT_ERROR;
	}
	for (i = 0; i < ncands; i++) {
		widx[i] = phasync_select_watch_add(p, cands[i].stream, cands[i].fd, slot, &watches[n]) ? n++ : -1;
	}

	/* The epfd itself, transient (stream == NULL, like any bare-fd wait), under
	 * the same slot: phasync_reg_arm() below (oneshot) is what actually makes
	 * poll() wake us for it; the watches above only ever wake us for a close. */
	er = zend_hash_index_find_ptr(&p->regs, (zend_ulong) epfd);
	if (er == NULL) {
		er = pecalloc(1, sizeof(*er), 1);
		er->fd = epfd;
		er->slot[0] = er->slot[1] = -1;
		er->sid[0] = er->sid[1] = -1;
		er->transient = true;
		zend_hash_index_add_new_ptr(&p->regs, (zend_ulong) epfd, er);
	}
	er->slot[0] = slot;
	p->armed++;
	if (phasync_reg_arm(p, er) != 0) {
		er->slot[0] = -1;
		p->armed--;
		zend_throw_error(NULL, "Unable to wait on the select() epoll instance: %s", strerror(errno));
		rc = PHASYNC_WAIT_ERROR;
	} else {
		GC_ADDREF(&p->std);
		rc = phasync_park(p, slot, timeout, timeout_ce);
		OBJ_RELEASE(&p->std);
	}

	/* Settle: the epfd's own registration first (as phasync_wait_settle() does
	 * for a transient bare-fd wait), then every watch that is not what woke
	 * us. A close leaves its watch's registration already gone (dropped by
	 * whichever of phasync_stream_forget()/phasync_socket_close_override() found
	 * it); anything else here is released, never dropped -- it may be a hooked
	 * stream's persistent registration, reused by its next real op. */
	er = zend_hash_index_find_ptr(&p->regs, (zend_ulong) epfd);
	if (er != NULL && er->slot[0] == slot) {
		er->slot[0] = -1;
		p->armed--;
	}
	if (er != NULL && er->transient && er->slot[0] < 0 && er->slot[1] < 0) {
		phasync_reg_drop(p, er);
	}

	*closed = -1;
	for (i = 0; i < ncands; i++) {
		if (widx[i] < 0) {
			continue;
		}
		if (phasync_select_watch_closed(p, &watches[widx[i]])) {
			if (*closed < 0 && (rc == PHASYNC_WAIT_READY || rc == PHASYNC_WAIT_TIMEOUT)) {
				*closed = i;
				rc = PHASYNC_WAIT_CLOSED;
			}
		} else {
			phasync_select_watch_release(p, &watches[widx[i]]);
		}
	}
	if (watches) {
		efree(watches);
		efree(widx);
	}
	return rc;
}

static void phasync_select_common(INTERNAL_FUNCTION_PARAMETERS, const char *fname,
		void (*orig)(INTERNAL_FUNCTION_PARAMETERS), php_socket_t (*fd_of)(zval *, php_stream **))
{
	zval *sets[3], saved[3], fn;
	zend_long sec = 0, usec = 0;
	bool secnull = 1, usecnull = 1;
	double deadline = 0;
	int i;

	if (PHASYNC_G(select_depth) || !phasync_reading() || EG(active_fiber) == NULL || phasync_from_c()) {
		orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZEND_PARSE_PARAMETERS_START(4, 5)
		Z_PARAM_ARRAY_EX2(sets[0], 1, 1, 0)
		Z_PARAM_ARRAY_EX2(sets[1], 1, 1, 0)
		Z_PARAM_ARRAY_EX2(sets[2], 1, 1, 0)
		Z_PARAM_LONG_OR_NULL(sec, secnull)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG_OR_NULL(usec, usecnull)
	ZEND_PARSE_PARAMETERS_END();

	/* A zero timeout never waits, and anything the original would reject (negative
	 * values, a null that isn't allowed) must fail exactly as it does natively. */
	if ((!secnull && sec == 0 && (usecnull || usec == 0))
	 || (!secnull && sec < 0) || (!usecnull && usec < 0)
	 || (secnull && !usecnull && usec != 0)
	 || (ZEND_NUM_ARGS() >= 5 && Z_TYPE_P(ZEND_CALL_ARG(execute_data, 5)) == IS_NULL
	     && strcmp(fname, "socket_select") == 0)) {
		orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	if (!secnull) {
		deadline = phasync_now() + (double) sec + (double) (usecnull ? 0 : usec) / 1000000.0;
	}

	for (i = 0; i < 3; i++) {
		if (sets[i]) {
			ZVAL_COPY(&saved[i], sets[i]);
		} else {
			ZVAL_NULL(&saved[i]);
		}
	}
	ZVAL_STRING(&fn, fname);

	for (;;) {
		zval args[5], ret, tmp;
		zval *savedp[3] = { &saved[0], &saved[1], &saved[2] };
		double remaining;
		int epfd, w;

		/* 1. zero-timeout probe of the original, on copies of the original arrays */
		for (i = 0; i < 3; i++) {
			ZVAL_COPY(&tmp, &saved[i]);
			ZVAL_NEW_REF(&args[i], &tmp);
		}
		ZVAL_LONG(&args[3], 0);
		ZVAL_LONG(&args[4], 0);
		ZVAL_UNDEF(&ret);
		PHASYNC_G(select_depth)++;
		call_user_function(NULL, NULL, &fn, &ret, 5, args);
		PHASYNC_G(select_depth)--;

		if (EG(exception) || Z_TYPE(ret) != IS_LONG || Z_LVAL(ret) != 0) {
			if (!EG(exception)) {
				for (i = 0; i < 3; i++) {
					if (sets[i]) {
						zval_ptr_dtor(sets[i]);
						ZVAL_COPY(sets[i], Z_REFVAL(args[i]));
					}
				}
				ZVAL_COPY(return_value, &ret);
			}
			for (i = 0; i < 5; i++) zval_ptr_dtor(&args[i]);
			zval_ptr_dtor(&ret);
			break;
		}
		for (i = 0; i < 5; i++) zval_ptr_dtor(&args[i]);
		zval_ptr_dtor(&ret);

		remaining = secnull ? INFINITY : deadline - phasync_now();
		if (remaining <= 0) {
			w = PHASYNC_WAIT_TIMEOUT;
		} else {
			/* 2. wait for any of them via one pollable epoll fd, or for a close of
			 * one of them (#16): a single safe pass builds both the epoll set and
			 * the candidates phasync_select_wait() watches for a close, so a
			 * watched stream's fd is never touched again once we might wait on it. */
			phasync_select_cand *cands;
			int ncands = 0, cap = 0, closed = -1;

			epfd = phasync_select_epoll(savedp, fd_of);
			if (epfd < 0) {
				/* nothing pollable to wait on: fall back to the native call */
				for (i = 0; i < 3; i++) zval_ptr_dtor(&saved[i]);
				zval_ptr_dtor(&fn);
				orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
				return;
			}
			for (i = 0; i < 3; i++) {
				if (Z_TYPE_P(savedp[i]) == IS_ARRAY) {
					cap += zend_hash_num_elements(Z_ARRVAL_P(savedp[i]));
				}
			}
			cands = cap ? safe_emalloc(cap, sizeof(*cands), 0) : NULL;
			for (i = 0; i < 3; i++) {
				zval *elem;
				if (Z_TYPE_P(savedp[i]) != IS_ARRAY) {
					continue;
				}
				zend_string *key_str;
				zend_ulong key_idx;
				ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(savedp[i]), key_idx, key_str, elem) {
					php_stream *s;
					php_socket_t fd = fd_of(elem, &s);
					if (fd < 0) {
						continue;
					}
					cands[ncands].fd      = fd;
					cands[ncands].stream  = s;
					cands[ncands].set     = i;
					cands[ncands].elem    = elem;
					cands[ncands].key_str = key_str;
					cands[ncands].key_idx = key_idx;
					ncands++;
				} ZEND_HASH_FOREACH_END();
			}
			w = phasync_select_wait(PHASYNC_G(scope_top)->poller, epfd, cands, ncands, remaining,
				PHASYNC_G(scope_top)->timeout_ce, &closed);
			close(epfd);
			if (w == PHASYNC_WAIT_CLOSED) {
				/* The closed descriptor is reported ready in whichever sets it was
				 * in (so the caller's next op on it fails as it would natively);
				 * everything else is dropped, exactly as a native timeout drops
				 * everything -- we do not know it is ready, only that it is not
				 * the one that closed. */
				php_socket_t cfd = cands[closed].fd;
				int n = 0;
				for (i = 0; i < 3; i++) {
					if (sets[i]) {
						zval_ptr_dtor(sets[i]);
						array_init(sets[i]);
					}
				}
				for (i = 0; i < ncands; i++) {
					if (cands[i].fd == cfd && sets[cands[i].set]) {
						HashTable *dst = Z_ARRVAL_P(sets[cands[i].set]);
						Z_TRY_ADDREF_P(cands[i].elem);
						if (cands[i].key_str) {
							zend_hash_update(dst, cands[i].key_str, cands[i].elem);
						} else {
							zend_hash_index_update(dst, cands[i].key_idx, cands[i].elem);
						}
						n++;
					}
				}
				RETVAL_LONG(n);
				efree(cands);
				break;
			}
			if (cands) {
				efree(cands);
			}
		}
		if (w == PHASYNC_WAIT_TIMEOUT) {
			for (i = 0; i < 3; i++) {       /* native timeout: 0, every array emptied */
				if (sets[i]) {
					zval_ptr_dtor(sets[i]);
					ZVAL_EMPTY_ARRAY(sets[i]);
				}
			}
			RETVAL_LONG(0);
			break;
		}
		if (w == PHASYNC_WAIT_ERROR) {
			break;                           /* exception pending: propagate */
		}
		/* 3. ready (or spurious): probe again */
	}
	for (i = 0; i < 3; i++) zval_ptr_dtor(&saved[i]);
	zval_ptr_dtor(&fn);
}

static ZEND_NAMED_FUNCTION(phasync_stream_select_override)
{
	phasync_select_common(INTERNAL_FUNCTION_PARAM_PASSTHRU, "stream_select",
		PHASYNC_G(orig_stream_select), phasync_select_fd_stream);
}

static ZEND_NAMED_FUNCTION(phasync_socket_select_override)
{
	phasync_select_common(INTERNAL_FUNCTION_PARAM_PASSTHRU, "socket_select",
		PHASYNC_G(orig_socket_select), phasync_select_fd_socket_ex);
}

/* ---- ext/sockets: socket_read() & co. -------------------------------------
 *
 * Socket objects are not streams, so their blocking calls are overridden one by
 * one. Inside a scope, in a fiber, on a socket left blocking: if it is not ready,
 * park until it is (its SO_RCVTIMEO/SO_SNDTIMEO, if set, as the native timeout),
 * then let the original do the call, which then doesn't wait; the real call
 * stays on PHP's thread, so a cancelled wait consumes nothing. When the native
 * timeout runs out, the original runs once non-blocking, failing with PHP's own
 * warning and socket error, as the native timeout does. socket_read() in
 * PHP_NORMAL_READ mode waits cooperatively for the first byte; the rest of the
 * line is read by the original. */

static const struct { const char *name; int dir; int timeo; } phasync_sock_funcs[] = {
	{"socket_read", PHASYNC_READ, SO_RCVTIMEO}, {"socket_recv", PHASYNC_READ, SO_RCVTIMEO},
	{"socket_recvfrom", PHASYNC_READ, SO_RCVTIMEO}, {"socket_accept", PHASYNC_READ, SO_RCVTIMEO},
	{"socket_write", PHASYNC_WRITE, SO_SNDTIMEO}, {"socket_send", PHASYNC_WRITE, SO_SNDTIMEO},
	{"socket_sendto", PHASYNC_WRITE, SO_SNDTIMEO},
};
#define PHASYNC_SOCK_NFUNCS (sizeof(phasync_sock_funcs) / sizeof(phasync_sock_funcs[0]))

/* The Socket argument's fd if it is a blocking socket we may wait on, else -1. */
static php_socket_t phasync_socket_arg(zend_execute_data *execute_data, phasync_php_socket **out)
{
	zval *z;
	php_socket_t fd;

	if (ZEND_NUM_ARGS() < 1 || (fd = phasync_select_fd_socket(z = ZEND_CALL_ARG(execute_data, 1))) == -1) {
		return -1;
	}
	ZVAL_DEREF(z);
	*out = (phasync_php_socket *) ((char *) Z_OBJ_P(z) - offsetof(phasync_php_socket, std));
	return (*out)->blocking ? fd : -1;
}

static ZEND_NAMED_FUNCTION(phasync_socket_io_override)
{
	/* Keyed by the name, not the function: a closure of it (socket_read(...),
	 * Closure::fromCallable()) calls a copy that shares the name (#9). */
	zval *idx = zend_hash_index_find(&PHASYNC_G(sock_hooks), (zend_ulong) (uintptr_t) EX(func)->common.function_name);
	zif_handler orig = PHASYNC_G(orig_sock)[Z_LVAL_P(idx)];
	int dir = phasync_sock_funcs[Z_LVAL_P(idx)].dir;
	phasync_php_socket *ps;
	php_socket_t fd;
	struct pollfd pfd;

	if (!phasync_reading() || EG(active_fiber) == NULL || (fd = phasync_socket_arg(execute_data, &ps)) == -1) {
		orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	pfd.fd = fd;
	pfd.events = dir == PHASYNC_READ ? POLLIN : POLLOUT;
	if (poll(&pfd, 1, 0) == 0) {
		struct timeval tv = {0, 0};
		socklen_t len = sizeof(tv);
		double timeout = INFINITY, deadline;
		int w;

		if (getsockopt(fd, SOL_SOCKET, phasync_sock_funcs[Z_LVAL_P(idx)].timeo, &tv, &len) == 0
		 && (tv.tv_sec || tv.tv_usec)) {
			timeout = (double) tv.tv_sec + tv.tv_usec / 1e6;
		}
		deadline = phasync_now() + timeout;
		do {
			w = phasync_wait_fd(dir, NULL, fd, isinf(timeout) ? INFINITY : MAX(deadline - phasync_now(), 0));
		} while (w == PHASYNC_WAIT_READY && poll(&pfd, 1, 0) == 0);
		if (w == PHASYNC_WAIT_ERROR) {
			return;                          /* exception pending */
		}
		if (w == PHASYNC_WAIT_TIMEOUT) {
			int flags = fcntl(fd, F_GETFL, 0);
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
			orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);   /* fails with the native EAGAIN */
			fcntl(fd, F_SETFL, flags);
			return;
		}
	}
	orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* What ext/sockets' PHP_SOCKET_ERROR() does: the socket's error, the module's
 * last error (the first field of its globals, the same from 8.2 to master; not
 * reachable this way in a ZTS build) and the warning. */
static void phasync_socket_error(phasync_php_socket *ps, const char *msg, int err)
{
	ps->error = err;
#ifndef ZTS
	{
		zend_module_entry *m = zend_hash_str_find_ptr(&module_registry, "sockets", sizeof("sockets") - 1);
		if (m && m->globals_ptr) {
			*(int *) m->globals_ptr = err;
		}
	}
#endif
	if (err != EAGAIN && err != EWOULDBLOCK && err != EINPROGRESS) {
		php_error_docref(NULL, E_WARNING, "%s [%d]: %s", msg, err, strerror(err));
	}
}

/* socket_close() while another coroutine waits on the socket: epoll forgets a
 * closed descriptor, so wake the waiters first (as a stream's close op does);
 * their retry finds the Socket closed, as PHP reports it. */
static ZEND_NAMED_FUNCTION(phasync_socket_close_override)
{
	zval *z = ZEND_NUM_ARGS() >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL;
	php_socket_t fd;

	if (z && (fd = phasync_select_fd_socket(z)) != -1) {
		for (phasync_poller *p = phasync_pollers; p; p = p->next) {
			phasync_reg *r = p->fork_gen == phasync_fork_gen ? zend_hash_index_find_ptr(&p->regs, (zend_ulong) fd) : NULL;
			if (r == NULL || r->stream != NULL) {
				continue;
			}
			for (int i = 0; i < 2; i++) {
				if (r->slot[i] >= 0) {
					phasync_chan_push(p->chan, r->slot[i]);
					p->armed -= PHASYNC_REG_WAITS(r, i);
					r->slot[i] = -1;
				}
			}
			phasync_reg_drop(p, r);
		}
	}
	PHASYNC_G(orig_socket_close)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* socket_connect() to a numeric IPv4/IPv6 address or a unix path: connect
 * non-blocking and park until the socket is writable; true on success, false
 * with ext/sockets' own error reporting otherwise, as the original does. A host
 * name goes to the original (its lookup is PHP's internal blocking one). */
static ZEND_NAMED_FUNCTION(phasync_socket_connect_override)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *zaddr = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;
	zval *zport = argc >= 3 ? ZEND_CALL_ARG(execute_data, 3) : NULL;
	struct sockaddr_storage ss;
	socklen_t sslen = 0;
	phasync_php_socket *ps;
	php_socket_t fd;
	int flags, rc, err = 0;

	memset(&ss, 0, sizeof(ss));
	if (!phasync_reading() || EG(active_fiber) == NULL || (fd = phasync_socket_arg(execute_data, &ps)) == -1
	 || !zaddr || Z_TYPE_P(zaddr) != IS_STRING || (zport && Z_TYPE_P(zport) != IS_LONG && Z_TYPE_P(zport) != IS_NULL)) {
		PHASYNC_G(orig_socket_connect)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	if (ps->type == AF_INET && zport && Z_TYPE_P(zport) == IS_LONG
	 && inet_pton(AF_INET, Z_STRVAL_P(zaddr), &((struct sockaddr_in *) &ss)->sin_addr) == 1) {
		((struct sockaddr_in *) &ss)->sin_family = AF_INET;
		((struct sockaddr_in *) &ss)->sin_port = htons((unsigned short) Z_LVAL_P(zport));
		sslen = sizeof(struct sockaddr_in);
	} else if (ps->type == AF_INET6 && zport && Z_TYPE_P(zport) == IS_LONG
	        && inet_pton(AF_INET6, Z_STRVAL_P(zaddr), &((struct sockaddr_in6 *) &ss)->sin6_addr) == 1) {
		((struct sockaddr_in6 *) &ss)->sin6_family = AF_INET6;
		((struct sockaddr_in6 *) &ss)->sin6_port = htons((unsigned short) Z_LVAL_P(zport));
		sslen = sizeof(struct sockaddr_in6);
	} else if (ps->type == AF_UNIX && Z_STRLEN_P(zaddr) < sizeof(((struct sockaddr_un *) &ss)->sun_path)) {
		((struct sockaddr_un *) &ss)->sun_family = AF_UNIX;
		memcpy(((struct sockaddr_un *) &ss)->sun_path, Z_STRVAL_P(zaddr), Z_STRLEN_P(zaddr));
		sslen = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + Z_STRLEN_P(zaddr));
	}
	if (sslen == 0) {
		PHASYNC_G(orig_socket_connect)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	do { rc = connect(fd, (struct sockaddr *) &ss, sslen); } while (rc != 0 && errno == EINTR);
	err = rc != 0 ? errno : 0;
	if (rc != 0 && errno == EINPROGRESS) {
		socklen_t elen = sizeof(err);
		if (phasync_wait_fd(PHASYNC_WRITE, NULL, fd, INFINITY) != PHASYNC_WAIT_READY) {
			fcntl(fd, F_SETFL, flags);
			return;                          /* exception pending */
		}
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0) {
			err = errno;
		}
		rc = err == 0 ? 0 : -1;
	}
	fcntl(fd, F_SETFL, flags);
	if (rc == 0) {
		RETURN_TRUE;
	}
	phasync_socket_error(ps, "unable to connect", err);
	RETURN_FALSE;
}

/* curl_multi_select() waits for curl's own sockets, which PHP doesn't expose
 * (no curl_multi_fdset()). Inside a scope, in a fiber: probe the original with a
 * zero timeout, and sleep through the loop between probes (1ms, doubling to
 * 20ms) until activity or the caller's timeout; 0 on timeout, as natively. The
 * probe calls the function again by name, which (select_depth > 0) goes
 * straight to the original. */
/* The cooperative wait itself; also curl_exec()'s, called directly (its own
 * frame would make the by-name call look like C calling PHP). */
static void phasync_curl_select(zval *zmh, double timeout, zval *return_value)
{
	double deadline = phasync_now() + timeout;
	zend_long usec = 1000;
	zval fn, args[2], ret;

	ZVAL_STRING(&fn, "curl_multi_select");
	ZVAL_COPY(&args[0], zmh);
	ZVAL_DOUBLE(&args[1], 0.0);
	for (;;) {
		double left;
		ZVAL_UNDEF(&ret);
		PHASYNC_G(select_depth)++;
		call_user_function(NULL, NULL, &fn, &ret, 2, args);
		PHASYNC_G(select_depth)--;
		if (EG(exception) || Z_TYPE(ret) != IS_LONG || Z_LVAL(ret) != 0
		 || (left = deadline - phasync_now()) <= 0) {
			break;                           /* activity, an error (-1), or time up */
		}
		zval_ptr_dtor(&ret);
		if (phasync_call_sleep(phasync_sleep_handler(), MIN(usec, (zend_long) (left * 1e6) + 1)) < 0) {
			ZVAL_UNDEF(&ret);
			break;                           /* exception pending */
		}
		usec = MIN(usec * 2, 20000);
	}
	zval_ptr_dtor(&fn);
	zval_ptr_dtor(&args[0]);
	if (!Z_ISUNDEF(ret)) {
		RETURN_COPY_VALUE(&ret);
	}
}

static ZEND_NAMED_FUNCTION(phasync_curl_multi_select_override)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval *zmh = argc >= 1 ? ZEND_CALL_ARG(execute_data, 1) : NULL;
	zval *zto = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;

	if (PHASYNC_G(select_depth) || !phasync_reading() || EG(active_fiber) == NULL || phasync_from_c() || !zmh
	 || (zto && Z_TYPE_P(zto) != IS_DOUBLE && Z_TYPE_P(zto) != IS_LONG)) {
		PHASYNC_G(orig_curl_multi_select)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	phasync_curl_select(zmh, zto ? zval_get_double(zto) : 1.0, return_value);
}

/* sem_acquire() and msg_receive() block in the kernel until the semaphore or a
 * message is there, with nothing to wait on. Inside a scope, in a fiber, their
 * non-blocking forms are retried from the loop instead (1ms, doubling to 20ms
 * between tries), so nothing is taken except on PHP's thread and a cancelled
 * wait takes nothing. The function is called again by name (select_depth > 0:
 * straight to the original) with the caller's arguments, references included. */
static bool phasync_call_again(const char *name, zval *retval, uint32_t argc, zval *args)
{
	zval fn;
	bool ok;

	ZVAL_STRING(&fn, name);
	ZVAL_UNDEF(retval);
	PHASYNC_G(select_depth)++;
	ok = call_user_function(NULL, NULL, &fn, retval, argc, args) == SUCCESS && !EG(exception);
	PHASYNC_G(select_depth)--;
	zval_ptr_dtor(&fn);
	return ok;
}

/* sem_acquire($sem) -> sem_acquire($sem, true) until it succeeds; EAGAIN (taken)
 * is the only failure that is retried (the native non-blocking form reports it
 * without a warning); any other failure returns as natively. */
static ZEND_NAMED_FUNCTION(phasync_sem_acquire_override)
{
	uint32_t argc = ZEND_NUM_ARGS();
	zval args[2], ret;
	zend_long usec = 1000;

	if (PHASYNC_G(select_depth) || !phasync_reading() || EG(active_fiber) == NULL || phasync_from_c() || argc < 1
	 || (argc >= 2 && zend_is_true(ZEND_CALL_ARG(execute_data, 2)))) {
		PHASYNC_G(orig_sem_acquire)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	ZVAL_COPY_VALUE(&args[0], ZEND_CALL_ARG(execute_data, 1));
	ZVAL_TRUE(&args[1]);
	for (;;) {
		errno = 0;
		if (!phasync_call_again("sem_acquire", &ret, 2, args) || Z_TYPE(ret) != IS_FALSE || errno != EAGAIN) {
			break;
		}
		if (phasync_call_sleep(phasync_sleep_handler(), usec) < 0) {
			return;                          /* exception pending */
		}
		usec = MIN(usec * 2, 20000);
	}
	if (!Z_ISUNDEF(ret)) {
		RETURN_COPY_VALUE(&ret);
	}
}

/* msg_receive(...) -> the same with MSG_IPC_NOWAIT until a message comes; "no
 * message" (ENOMSG in $error_code, no warning) is the only result retried. */
static ZEND_NAMED_FUNCTION(phasync_msg_receive_override)
{
	uint32_t argc = ZEND_NUM_ARGS(), i;
	zval args[8], ret, errcode, *uflags = argc >= 7 ? ZEND_CALL_ARG(execute_data, 7) : NULL;
	zend_long usec = 1000;
	bool own_err = argc < 8;

	if (PHASYNC_G(select_depth) || !phasync_reading() || EG(active_fiber) == NULL || phasync_from_c() || argc < 5
	 || (uflags && (Z_TYPE_P(uflags) != IS_LONG || (Z_LVAL_P(uflags) & 1 /* MSG_IPC_NOWAIT */)))) {
		PHASYNC_G(orig_msg_receive)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	for (i = 0; i < argc; i++) {
		ZVAL_COPY_VALUE(&args[i], ZEND_CALL_ARG(execute_data, i + 1));
	}
	if (argc < 6) {
		ZVAL_TRUE(&args[5]);                 /* $unserialize default */
	}
	ZVAL_LONG(&args[6], (uflags ? Z_LVAL_P(uflags) : 0) | 1);
	if (own_err) {
		ZVAL_NULL(&errcode);
		ZVAL_NEW_REF(&args[7], &errcode);
	}
	for (;;) {
		zval *code;
		if (!phasync_call_again("msg_receive", &ret, 8, args) || Z_TYPE(ret) != IS_FALSE) {
			break;
		}
		code = &args[7];
		ZVAL_DEREF(code);
		if (Z_TYPE_P(code) != IS_LONG || Z_LVAL_P(code) != ENOMSG) {
			break;
		}
		if (phasync_call_sleep(phasync_sleep_handler(), usec) < 0) {
			ZVAL_UNDEF(&ret);
			break;                           /* exception pending */
		}
		usec = MIN(usec * 2, 20000);
	}
	if (own_err) {
		zval_ptr_dtor(&args[7]);
	}
	if (!Z_ISUNDEF(ret)) {
		RETURN_COPY_VALUE(&ret);
	}
}

/* Call a PHP function by name (through any override of ours). */
static bool phasync_call_named(const char *name, zval *retval, uint32_t argc, zval *args)
{
	zval fn;
	bool ok;

	ZVAL_STRING(&fn, name);
	ZVAL_UNDEF(retval);
	ok = call_user_function(NULL, NULL, &fn, retval, argc, args) == SUCCESS && !EG(exception);
	zval_ptr_dtor(&fn);
	return ok;
}

/* curl_exec() inside a scope, in a fiber: the same transfer on a private
 * curl_multi handle, waiting in the cooperative curl_multi_select(), so option
 * callbacks run on PHP's thread, in the coroutine. The result is native:
 * curl_multi_info_read() records the transfer's error on the handle (curl_errno,
 * curl_error), adding the handle resets it as curl_exec() does, and the return is
 * false on error, the body with CURLOPT_RETURNTRANSFER, else true; FILE* outputs
 * are flushed, as curl_exec() does. A handle already in a multi (or a multi
 * error) goes to the original, which reports it natively. A cancellation drops
 * the transfer and propagates. */
static ZEND_NAMED_FUNCTION(phasync_curl_exec_override)
{
	zval mh, args[2], ret, running, info;
	zend_long result = 0;
	bool added = false;

	if (!phasync_reading() || EG(active_fiber) == NULL || phasync_from_c() || ZEND_NUM_ARGS() != 1
	 || Z_TYPE_P(ZEND_CALL_ARG(execute_data, 1)) != IS_OBJECT
	 || !phasync_call_named("curl_multi_init", &mh, 0, NULL) || Z_TYPE(mh) != IS_OBJECT) {
		if (!EG(exception)) {
			PHASYNC_G(orig_curl_exec)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		}
		return;
	}
	ZVAL_COPY_VALUE(&args[0], &mh);
	ZVAL_COPY_VALUE(&args[1], ZEND_CALL_ARG(execute_data, 1));
	if (!phasync_call_named("curl_multi_add_handle", &ret, 2, args) || Z_TYPE(ret) != IS_LONG || Z_LVAL(ret) != 0) {
		goto native;
	}
	added = true;
	ZVAL_NULL(&running);
	ZVAL_NEW_REF(&args[1], &running);
	for (;;) {
		zval *r;
		zend_long rc;
		if (!phasync_call_named("curl_multi_exec", &ret, 2, args)) {
			goto cancelled;
		}
		rc = Z_TYPE(ret) == IS_LONG ? Z_LVAL(ret) : 1;
		if (rc != 0 && rc != -1 /* CURLM_CALL_MULTI_PERFORM */) {
			zval_ptr_dtor(&args[1]);
			ZVAL_COPY_VALUE(&args[1], ZEND_CALL_ARG(execute_data, 1));
			goto native;
		}
		r = Z_REFVAL(args[1]);
		if (Z_TYPE_P(r) != IS_LONG || Z_LVAL_P(r) == 0) {
			break;
		}
		ZVAL_UNDEF(&ret);
		phasync_curl_select(&mh, 1.0, &ret);
		if (EG(exception)) {
			goto cancelled;
		}
		zval_ptr_dtor(&ret);
	}
	zval_ptr_dtor(&args[1]);
	ZVAL_COPY_VALUE(&args[1], ZEND_CALL_ARG(execute_data, 1));
	if (phasync_call_named("curl_multi_info_read", &info, 1, &mh) && Z_TYPE(info) == IS_ARRAY) {
		zval *res = zend_hash_str_find(Z_ARRVAL(info), "result", sizeof("result") - 1);
		result = res ? zval_get_long(res) : 0;
	}
	zval_ptr_dtor(&info);
	phasync_call_named("curl_multi_remove_handle", &ret, 2, args);
	zval_ptr_dtor(&ret);
	fflush(NULL);                            /* CURLOPT_FILE/WRITEHEADER outputs */
	if (result != 0) {
		zval_ptr_dtor(&mh);
		RETURN_FALSE;
	}
	phasync_call_named("curl_multi_getcontent", &ret, 1, &args[1]);
	zval_ptr_dtor(&mh);
	if (Z_TYPE(ret) == IS_STRING) {
		RETURN_COPY_VALUE(&ret);
	}
	zval_ptr_dtor(&ret);
	RETURN_TRUE;

native:
	zval_ptr_dtor(&ret);
	if (added) {
		phasync_call_named("curl_multi_remove_handle", &ret, 2, args);
		zval_ptr_dtor(&ret);
	}
	zval_ptr_dtor(&mh);
	PHASYNC_G(orig_curl_exec)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	return;

cancelled:
	zval_ptr_dtor(&args[1]);
	ZVAL_COPY_VALUE(&args[1], ZEND_CALL_ARG(execute_data, 1));
	zend_object *saved_exc = EG(exception);  /* clean up with the exception set aside */
	EG(exception) = NULL;
	phasync_call_named("curl_multi_remove_handle", &ret, 2, args);
	zval_ptr_dtor(&ret);
	if (EG(exception)) {
		zend_exception_set_previous(EG(exception), saved_exc);
	} else {
		EG(exception) = saved_exc;
	}
	zval_ptr_dtor(&mh);
}

/* ---- module lifecycle ---------------------------------------------------- */

static void phasync_hook_entry_dtor(zval *zv)
{
	pefree(Z_PTR_P(zv), 1);
}
static void phasync_ops_dtor(zval *zv)
{
	pefree(Z_PTR_P(zv), 1);
}

/* ---- echo/print to the CLI's stdout -----------------------------------------
 *
 * The CLI SAPI writes output straight to fd 1, so echo blocks the whole process
 * on a full pipe (a slow log reader). Inside a scope, in a fiber, wait for fd 1 to
 * be writable and let the SAPI write at most PIPE_BUF bytes at a time, which then
 * can't block. Another fiber's output meanwhile waits its turn, so each echo stays
 * contiguous as it is natively. */
static size_t (*phasync_ub_write_orig)(const char *str, size_t len);

static size_t phasync_ub_write(const char *str, size_t len)
{
	int dir = phasync_writing();
	size_t done = 0, n;

	if (!dir || EG(active_fiber) == NULL) {
		return phasync_ub_write_orig(str, len);
	}
	while (PHASYNC_G(ub_writing)) {
		if (phasync_call_sleep(phasync_sleep_handler(), 1000) < 0) {
			return 0;                    /* exception pending */
		}
	}
	PHASYNC_G(ub_writing) = true;
	while (done < len) {
		struct pollfd pfd = { STDOUT_FILENO, POLLOUT, 0 };
		if (poll(&pfd, 1, 0) == 0) {
			if (phasync_wait_fd(dir, NULL, STDOUT_FILENO, INFINITY) != PHASYNC_WAIT_READY) {
				break;
			}
			continue;
		}
		if ((n = phasync_ub_write_orig(str + done, MIN(len - done, PIPE_BUF))) == 0) {
			break;
		}
		done += n;
	}
	PHASYNC_G(ub_writing) = false;
	return done;
}

/* stream_socket_pair() builds its sockets with socketpair(2) via
 * php_stream_sock_open_from_socket(), bypassing the transport factory — so wrap
 * both returned streams here (RAW; they are ordinary sockets). Wrapped
 * unconditionally, like the factory path: the wrapper is inert outside a scope. */
static ZEND_NAMED_FUNCTION(phasync_stream_socket_pair_override)
{
	PHASYNC_G(orig_stream_socket_pair)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (Z_TYPE_P(return_value) == IS_ARRAY) {
		zval *el;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(return_value), el) {
			if (Z_TYPE_P(el) == IS_RESOURCE) {
				php_stream *s = NULL;
				php_stream_from_zval_no_verify(s, el);
				if (s) {
					phasync_wrap_stream(s, PHASYNC_MODE_RAW);
				}
			}
		} ZEND_HASH_FOREACH_END();
	}
}

/* fclose() in a coroutine being destroyed (its finally blocks), of a stream other
 * coroutines are inside an op on: it cannot wait for them, and its close op would
 * free the stream under them. phasync_close_if_unwinding() closes it as far as
 * they are concerned (woken, their ops fail with EBADF) and leaves the stream to
 * its last reference, which they hold (#14, #23). */
static ZEND_NAMED_FUNCTION(phasync_fclose_override)
{
	zval *res;
	php_stream *stream;

	if (phasync_unwinding() && ZEND_NUM_ARGS() == 1
	 && Z_TYPE_P(res = ZEND_CALL_ARG(execute_data, 1)) == IS_RESOURCE
	 && (stream = zend_fetch_resource2(Z_RES_P(res), NULL, php_file_le_stream(), php_file_le_pstream()))
	 && !(stream->flags & PHP_STREAM_FLAG_NO_FCLOSE)
	 && phasync_close_if_unwinding(stream)) {
		RETURN_TRUE;
	}
	PHASYNC_G(orig_fclose)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* pclose() there, of a stream other coroutines are inside an op on: the same
 * (#23). zend_list_close() below would otherwise free it under them exactly as
 * fclose() would; pclose() returns the child's exit status, which is not
 * available without waiting for it (impossible here), so -1 as on a pclose()
 * of something that was never a valid process stream. */
static ZEND_NAMED_FUNCTION(phasync_pclose_override)
{
	zval *res;
	php_stream *stream;

	if (phasync_unwinding() && ZEND_NUM_ARGS() == 1
	 && Z_TYPE_P(res = ZEND_CALL_ARG(execute_data, 1)) == IS_RESOURCE
	 && (stream = zend_fetch_resource2(Z_RES_P(res), NULL, php_file_le_stream(), php_file_le_pstream()))
	 && phasync_close_if_unwinding(stream)) {
		RETURN_LONG(-1);
	}
	PHASYNC_G(orig_pclose)(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
