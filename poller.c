/* Included by phasync.c (single translation unit); not compiled on its own. */

/* Poller object: epoll/select drivers, getSlot/park/unpark waiting, the select()/socket_select() override's shared wait path, inflight tracking. */

/* ---- waiting: phasync\ext\Poller -------------------------------------------
 *
 * A Poller belongs to an event loop, which gives it its getSlot(), park() and
 * unpark(). A coroutine waits by parking in a fresh slot; the Poller resumes it
 * by unparking the slot, and only ever on PHP's thread, inside its poll(). A late
 * wake-up can only find a vacant slot (unpark() returns false).
 *
 * A stream with a close hook keeps a level-triggered registration, armed for a
 * direction on its first wait and left armed after it: in request/response I/O
 * nothing arrives while the coroutine is busy, so the next wait needs no
 * syscall. An event that finds nobody waiting disarms its direction (MOD) there
 * and then, so unread data with nobody waiting costs one wake-up, never a busy
 * loop; a hang-up with nobody waiting deletes the registration (epoll reports
 * hang-ups whatever the interest). Each direction reuses one slot, unparked
 * only from its registration. Other descriptors get a one-shot registration
 * (PHASYNC_EV_ONESHOT), re-armed on every wait, and a fresh slot. A registration lives as
 * long as its stream, which is deregistered in its close op (epoll keeps a
 * registration while any duplicate of the descriptor lives, so close() alone is
 * not enough). A stream without a close hook keeps its registration too, until
 * another stream turns up with its descriptor number and takes it over (epoll
 * dropped it when the file closed; a stale one can only cause a spurious
 * wake-up). Bare descriptors get a registration for the one wait only.
 *
 * The thread tasks of the hooked operations under a manage() given this Poller
 * report to its completion channel: they queue the slot to wake and write to an
 * eventfd in the epoll set, never calling PHP; poll() drains the queue. The
 * channel is reference counted, so a task still running when its Poller is freed
 * finds a live channel. */


static zend_class_entry *phasync_poller_ce;
static zend_object_handlers phasync_poller_handlers;
static phasync_poller *phasync_pollers;   /* live Pollers of this process */



/* Bumped in every fork()ed child (pthread_atfork), so a Poller can tell it is
 * used from a process other than its creator without a getpid() syscall. */
static unsigned phasync_fork_gen;

static void phasync_preempt_fork_prepare(void);
static void phasync_preempt_fork_parent(void);
static void phasync_preempt_fork_child(void);

static void phasync_fork_child(void)
{
	phasync_fork_gen++;
	phasync_preempt_fork_child();
}

static zend_always_inline phasync_poller *phasync_poller_from(zend_object *obj)
{
	return (phasync_poller *) ((char *) obj - offsetof(phasync_poller, std));
}

static void phasync_reg_dtor(zval *zv)
{
	pefree(Z_PTR_P(zv), 1);
}

/* A private kernel queue, itself pollable: readable once any descriptor added
 * to it is ready for what it was added for (PHASYNC_EV_IN/PRI/OUT). Regular
 * files are refused, as epoll_ctl() does. */
static int phasync_mux_open(void)
{
#ifdef __linux__
	return epoll_create1(EPOLL_CLOEXEC);
#else
	int fd = kqueue();
	if (fd >= 0) {
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	}
	return fd;
#endif
}

static int phasync_mux_add(int mux, int fd, uint32_t events)
{
#ifdef __linux__
	struct epoll_event e = { .events = events, .data.fd = fd };
	return epoll_ctl(mux, EPOLL_CTL_ADD, fd, &e);
#else
	struct kevent ch[2];
	struct stat st;
	int n = 0;

	if (fstat(fd, &st) != 0 || S_ISREG(st.st_mode)) {
		return -1;
	}
	if (events & (PHASYNC_EV_IN | PHASYNC_EV_PRI)) {
		EV_SET(&ch[n++], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	}
	if (events & PHASYNC_EV_OUT) {
		EV_SET(&ch[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
	}
	return kevent(mux, ch, n, NULL, 0, NULL) < 0 ? -1 : 0;
#endif
}

/* The thread pool's completion channel: an eventfd on Linux, a pipe elsewhere.
 * Any thread signals it; poll() drains it. */
static bool phasync_chan_open(phasync_chan *ch)
{
#ifdef __linux__
	ch->evfd = ch->evfd_w = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	return ch->evfd >= 0;
#else
	int fds[2];
	if (pipe(fds) != 0) {
		ch->evfd = ch->evfd_w = -1;
		return false;
	}
	for (int i = 0; i < 2; i++) {
		fcntl(fds[i], F_SETFL, O_NONBLOCK);
		fcntl(fds[i], F_SETFD, FD_CLOEXEC);
	}
	ch->evfd = fds[0];
	ch->evfd_w = fds[1];
	return true;
#endif
}

static void phasync_chan_close(phasync_chan *ch)
{
	close(ch->evfd);
	if (ch->evfd_w != ch->evfd) {
		close(ch->evfd_w);
	}
}

static void phasync_chan_signal(phasync_chan *ch)
{
	uint64_t one = 1;
	ssize_t w;

	do { w = write(ch->evfd_w, &one, ch->evfd_w == ch->evfd ? sizeof(one) : 1); } while (w < 0 && errno == EINTR);
}

static void phasync_chan_drain(phasync_chan *ch)
{
	uint64_t buf[8];

	(void) !read(ch->evfd, buf, ch->evfd_w == ch->evfd ? sizeof(buf[0]) : sizeof(buf));
}

static void phasync_chan_release(phasync_chan *ch)
{
	int refs;

	pthread_mutex_lock(&ch->mutex);
	refs = --ch->refs;
	pthread_mutex_unlock(&ch->mutex);
	if (refs == 0) {
		phasync_chan_close(ch);
		free(ch->queue);
		pthread_mutex_destroy(&ch->mutex);
		free(ch);
	}
}

/* Queue a slot for poll() to unpark, from any thread. */
static void phasync_chan_push(phasync_chan *ch, zend_long slot)
{
	pthread_mutex_lock(&ch->mutex);
	if (ch->len == ch->cap) {
		size_t cap = ch->cap ? ch->cap * 2 : 64;
		zend_long *q = realloc(ch->queue, cap * sizeof(*q));
		if (q) {
			ch->queue = q;
			ch->cap = cap;
		}
	}
	if (ch->len < ch->cap) {
		ch->queue[ch->len++] = slot;
	}
	pthread_mutex_unlock(&ch->mutex);
	phasync_chan_signal(ch);
}

/* Take back a slot's queued wake-up: its coroutine stopped waiting (cancelled,
 * timed out, or destroyed), and poll() must not unpark it. */
static void phasync_chan_purge(phasync_chan *ch, zend_long slot)
{
	size_t j = 0;

	pthread_mutex_lock(&ch->mutex);
	for (size_t i = 0; i < ch->len; i++) {
		if (ch->queue[i] != slot) {
			ch->queue[j++] = ch->queue[i];
		}
	}
	ch->len = j;
	pthread_mutex_unlock(&ch->mutex);
}

/* ---- the ledger: what the extension holds for a suspended coroutine --------
 *
 * While a coroutine is suspended inside a hooked operation, the extension holds
 * things for it: a registration and slot in a Poller, a count in the stream's
 * in-flight record (#14), a pool operation, a closer's wait. Each is entered in
 * the coroutine's ledger before it suspends and struck off by the code after the
 * suspension. When PHP destroys a suspended fiber it resumes it once to unwind it,
 * and that code settles everything; whatever it never gets back to (a fatal error
 * unwinding past it) the fiber's destroy observer settles, and at the end of the
 * request whatever is left. None of it depends on PHP code (the application's
 * finally blocks, the loop's bookkeeping): a slot is taken back from the Poller
 * before its coroutine is gone, so the extension never wakes a dead one. */
struct phasync_task;

static phasync_ledger *phasync_ledger_cur(void)
{
	zend_ulong key = (zend_ulong) (uintptr_t) EG(current_fiber_context);
	phasync_ledger *l = zend_hash_index_find_ptr(&PHASYNC_G(ledgers), key);

	if (l == NULL) {
		l = pecalloc(1, sizeof(*l), 1);
		zend_hash_index_add_new_ptr(&PHASYNC_G(ledgers), key, l);
	}
	return l;
}

/* Throw unless this process created the Poller: in a fork()ed child its epoll
 * and eventfd instances are the parent's. */
static bool phasync_poller_usable(phasync_poller *p)
{
	if (p->fork_gen == phasync_fork_gen) {
		return true;
	}
	zend_throw_error(NULL, "This Poller belongs to another process; create a new one after fork()");
	return false;
}

/* A new slot from the loop's getSlot(); -1 if it threw. */
static zend_long phasync_get_slot(phasync_poller *p)
{
	zval retval;
	zend_long slot = -1;

	ZVAL_UNDEF(&retval);
	if (call_user_function(NULL, NULL, &p->get_slot, &retval, 0, NULL) == SUCCESS && !EG(exception)) {
		slot = zval_get_long(&retval);
	}
	zval_ptr_dtor(&retval);
	return slot;
}

/* Park the current coroutine in $slot. READY when unparked. With a timeout
 * class (the manage() scope's, for a hooked call with a native timeout), park()
 * throwing it once the timeout given has run out clears the exception: TIMEOUT,
 * finish the op the way PHP does on a timeout. Otherwise (the public
 * readable()/writable(), or any other exception, a cancellation) it stays
 * pending: ERROR. */
static int phasync_park(phasync_poller *p, zend_long slot, double timeout, zend_class_entry *timeout_ce)
{
	zval args[2], retval;
	double deadline = isinf(timeout) ? INFINITY : phasync_now() + timeout;
	int rc = PHASYNC_WAIT_READY;

	if (phasync_cannot_suspend()) {
		return PHASYNC_WAIT_ERROR;
	}
	ZVAL_LONG(&args[0], slot);
	ZVAL_DOUBLE(&args[1], isinf(timeout) ? DBL_MAX : timeout);
	ZVAL_UNDEF(&retval);
	if (call_user_function(NULL, NULL, &p->park, &retval, 2, args) == FAILURE) {
		rc = PHASYNC_WAIT_ERROR;
	} else if (EG(exception)) {
		if (timeout_ce && !isinf(deadline) && instanceof_function(EG(exception)->ce, timeout_ce)
		 && phasync_now() + 0.001 >= deadline) {
			zend_clear_exception();
			rc = PHASYNC_WAIT_TIMEOUT;
		} else {
			rc = PHASYNC_WAIT_ERROR;
		}
	}
	zval_ptr_dtor(&retval);
	if (rc != PHASYNC_WAIT_READY) {
		phasync_chan_purge(p->chan, slot);
	}
	return rc;
}

/* ---- backend seam: the kernel queue (epoll, kqueue) is touched only through
 * these and phasync_poller_epoll_wait(); IOCP would replace them. */

#ifdef __linux__
static bool phasync_backend_create(phasync_poller *p)
{
	p->epfd = epoll_create1(EPOLL_CLOEXEC);
	return p->epfd >= 0;
}
static void phasync_backend_destroy(phasync_poller *p)
{
	if (p->epfd >= 0) {
		close(p->epfd);
	}
}

/* fd's interest becomes exactly `events` (idempotent: ADD or MOD as needed).
 * *added tracks whether the backend currently knows fd, flipped to true on
 * success; the caller owns that flag (one per registration). */
static int phasync_backend_set(phasync_poller *p, php_socket_t fd, uint32_t events, bool *added)
{
	struct epoll_event e;

	e.events = events;
	e.data.u64 = (uint64_t) fd;
	if (epoll_ctl(p->epfd, *added ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &e) != 0) {
		/* A taken-over registration: epoll may have dropped it (the file closed), or
		 * the descriptor may be another file it has never seen. */
		if (!(errno == ENOENT && *added) && !(errno == EEXIST && !*added)) {
			return -1;
		}
		if (epoll_ctl(p->epfd, *added ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &e) != 0) {
			return -1;
		}
	}
	*added = true;
	return 0;
}

/* Stop watching fd; a no-op if the backend doesn't currently know it. */
static void phasync_backend_remove(phasync_poller *p, php_socket_t fd, bool *added)
{
	if (*added) {
		epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL);
		*added = false;
	}
}

static int phasync_backend_wait(phasync_poller *p, phasync_event *ev, int maxevents, int ms)
{
	return epoll_wait(p->epfd, ev, maxevents, ms);
}

#else
static bool phasync_backend_create(phasync_poller *p)
{
	p->epfd = kqueue();
	if (p->epfd >= 0) {
		fcntl(p->epfd, F_SETFD, FD_CLOEXEC);
	}
	return p->epfd >= 0;
}

static void phasync_backend_destroy(phasync_poller *p)
{
	if (p->epfd >= 0) {
		close(p->epfd);
	}
}

/* The read and write filters become exactly `events`; ONESHOT disables a filter
 * once it fired (EV_DISPATCH), as epoll's. A regular file is refused (EPERM),
 * as epoll_ctl() does: kqueue would accept it but never report EOF. */
static int phasync_backend_set(phasync_poller *p, php_socket_t fd, uint32_t events, bool *added)
{
	struct kevent ch[2], res[2];
	unsigned short once = events & PHASYNC_EV_ONESHOT ? EV_DISPATCH : 0;
	int n;

	if (!*added) {
		struct stat st;
		if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
			errno = EPERM;
			return -1;
		}
	}
	EV_SET(&ch[0], fd, EVFILT_READ, (events & (PHASYNC_EV_IN | PHASYNC_EV_RDHUP) ? EV_ADD | EV_ENABLE | once : EV_DELETE) | EV_RECEIPT, 0, 0, NULL);
	EV_SET(&ch[1], fd, EVFILT_WRITE, (events & PHASYNC_EV_OUT ? EV_ADD | EV_ENABLE | once : EV_DELETE) | EV_RECEIPT, 0, 0, NULL);
	n = kevent(p->epfd, ch, 2, res, 2, NULL);
	if (n < 0) {
		return -1;
	}
	for (int i = 0; i < n; i++) {
		if ((res[i].flags & EV_ERROR) && res[i].data != 0 && res[i].data != ENOENT) {
			errno = (int) res[i].data;
			return -1;
		}
	}
	*added = true;
	return 0;
}

static void phasync_backend_remove(phasync_poller *p, php_socket_t fd, bool *added)
{
	if (*added) {
		struct kevent ch[2], res[2];
		EV_SET(&ch[0], fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
		EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
		kevent(p->epfd, ch, 2, res, 2, NULL);
		*added = false;
	}
}

/* kevents as epoll events: one per filter (a descriptor ready both ways comes
 * twice), EOF as RDHUP on the read side and HUP on the write side. */
static int phasync_backend_wait(phasync_poller *p, phasync_event *ev, int maxevents, int ms)
{
	struct kevent kev[256];
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	int n = kevent(p->epfd, NULL, 0, kev, maxevents < 256 ? maxevents : 256, ms < 0 ? NULL : &ts);

	for (int i = 0; i < n; i++) {
		bool rd = kev[i].filter == EVFILT_READ;
		ev[i].data.u64 = (uint64_t) kev[i].ident;
		ev[i].events = (rd ? PHASYNC_EV_IN : PHASYNC_EV_OUT)
		             | (kev[i].flags & EV_EOF ? (rd ? PHASYNC_EV_RDHUP : PHASYNC_EV_HUP) : 0)
		             | (kev[i].flags & EV_ERROR ? PHASYNC_EV_ERR : 0);
	}
	return n;
}
#endif

/* Arm the registration for whoever is parked on it (one-shot); with nobody,
 * leave it disarmed. Returns -1 (errno set) if the backend refused the descriptor. */
static int phasync_reg_ctl(phasync_poller *p, phasync_reg *r, uint32_t ev, bool oneshot)
{
	if (phasync_backend_set(p, r->fd, ev | (oneshot ? PHASYNC_EV_ONESHOT : 0), &r->added) != 0) {
		return -1;
	}
	r->events = ev;
	return 0;
}

/* Make the registration cover whoever is parked on it: a hooked one only ever
 * gains interest here (a no-op costs no syscall), a one-shot one is re-armed. */
static int phasync_reg_arm(phasync_poller *p, phasync_reg *r)
{
	uint32_t want = (PHASYNC_REG_WAITS(r, 0) ? PHASYNC_EV_IN | PHASYNC_EV_RDHUP : 0) | (PHASYNC_REG_WAITS(r, 1) ? PHASYNC_EV_OUT : 0);

	if (p->select_mode) {
		return 0;   /* select() builds its sets from the slots on every poll() */
	}
	if (r->hooked) {
		return r->added && (r->events | want) == r->events ? 0 : phasync_reg_ctl(p, r, r->events | want, false);
	}
	return phasync_reg_ctl(p, r, want, true);
}

/* Below this many watched descriptors the Poller waits with select(); at it,
 * it switches to epoll for the rest of the process. Measured on black: select()
 * costs ~25 ns per watched fd per call, epoll ~1 us (ADD+DEL) per connection
 * plus 87 ns per epoll_wait(). So select() wins under churn and at low counts
 * (busy churn favoured it up to 5000); at 1024 a mostly idle set costs ~26 us
 * per wake-up, and above that the idle (long-poll) penalty grows unbounded. */
#define PHASYNC_SELECT_THRESHOLD 1024

/* Bytes for a descriptor bitmap holding bit `maxfd`, in whole unsigned longs:
 * the kernel reads and writes the sets in words. */
#define PHASYNC_FDSET_BYTES(maxfd) ((((size_t) (maxfd) / (8 * sizeof(unsigned long))) + 1) * sizeof(unsigned long))

/* glibc's FD_SET/FD_ISSET are fortified (_FORTIFY_SOURCE) to abort past the
 * nominal FD_SETSIZE regardless of how big the buffer actually allocated
 * is, so a past-FD_SETSIZE bitmap needs its bits set and read by hand. */
#define PHASYNC_FD_SET(fd, buf)   (((unsigned long *) (buf))[(size_t) (fd) / (8 * sizeof(unsigned long))] |= (1UL << ((size_t) (fd) % (8 * sizeof(unsigned long)))))
#define PHASYNC_FD_ISSET(fd, buf) ((((unsigned long *) (buf))[(size_t) (fd) / (8 * sizeof(unsigned long))] >> ((size_t) (fd) % (8 * sizeof(unsigned long)))) & 1UL)

/* The watched count reached the threshold: epoll from now on, for good. Arm
 * every parked wait (idle registrations arm on their next wait). */
static void phasync_poller_use_epoll(phasync_poller *p)
{
	phasync_reg *r;

	p->select_mode = false;
	ZEND_HASH_FOREACH_PTR(&p->regs, r) {
		if ((PHASYNC_REG_WAITS(r, 0) || PHASYNC_REG_WAITS(r, 1)) && phasync_reg_arm(p, r) != 0) {
			/* A regular file, parked under select() (which always reports one
			 * ready): epoll_ctl() refuses it (#29). Wake its waiter(s) at the
			 * next poll(), same as a regular file waited on directly in epoll
			 * mode -- there's nobody left on this call stack to hand a result
			 * to directly, this reg's coroutine(s) are already parked. */
			for (int i = 0; i < 2; i++) {
				if (PHASYNC_REG_WAITS(r, i)) {
					phasync_chan_push(p->chan, r->slot[i]);
				}
			}
		}
	} ZEND_HASH_FOREACH_END();
}

static void phasync_reg_drop(phasync_poller *p, phasync_reg *r)
{
	phasync_backend_remove(p, r->fd, &r->added);
	zend_hash_index_del(&p->regs, (zend_ulong) r->fd);   /* frees r */
}

/* A stream is closing: deregister it (from every live Poller) before its
 * descriptor goes away, and wake whoever is parked on it (through poll(), like
 * any wake-up); their next use of the stream finds it closed. */
static void phasync_stream_forget(php_stream *stream)
{
	php_socket_t fd = -2;

	if (stream == PHASYNC_G(fs_last)) {
		PHASYNC_G(fs_last) = NULL;
	}

	for (phasync_poller *p = phasync_pollers; p; p = p->next) {
		phasync_reg *r;

		if (zend_hash_num_elements(&p->regs) == 0) {
			continue;
		}
		if (fd == -2) {
			fd = phasync_stream_fd(stream);
		}
		if (fd == -1 || p->fork_gen != phasync_fork_gen
		 || (r = zend_hash_index_find_ptr(&p->regs, (zend_ulong) fd)) == NULL || r->stream != stream) {
			continue;
		}
		for (int i = 0; i < 2; i++) {
			if (r->slot[i] >= 0) {
				phasync_chan_push(p->chan, r->slot[i]);
				p->armed -= PHASYNC_REG_WAITS(r, i);
			}
		}
		phasync_reg_drop(p, r);
	}
}

static bool phasync_has_close_hook(php_stream *stream);

/* A wait on a registration is over (or its coroutine gone): if nobody unparked
 * it, disarm it, and let go of the Poller. The stream may have been closed
 * meanwhile (its registration freed): look it up again. */
static void phasync_wait_settle(phasync_poller *p, int idx, php_stream *stream, php_socket_t fd,
                                zend_long slot, int dupfd)
{
	phasync_reg *r = zend_hash_index_find_ptr(&p->regs, (zend_ulong) fd);

	if (r && r->stream != stream) {
		r = NULL;
	}
	if (r && r->slot[idx] == slot) {
		r->slot[idx] = -1;
		p->armed--;
		phasync_reg_arm(p, r);
	}
	if (r && r->transient && r->slot[0] < 0 && r->slot[1] < 0) {
		phasync_reg_drop(p, r);
	}
	if (dupfd >= 0) {
		close(dupfd);
	}
	OBJ_RELEASE(&p->std);
}

/* ---- PHP invoked from C: may it suspend? -------------------------------
 *
 * Yes, unless the stack holds something on the never-list (docs/c-to-php-suspension.md
 * has the reason for each entry and what takes it off): the callback of a C caller
 * that keeps process-global state across the call, whose recursion guard another
 * coroutine would trip over, or that the engine forbids to switch in. Those run
 * inline (blocking). Checked only at a suspension point or a preemption checkpoint,
 * from the current frame down to the fiber's first. The preempt function a
 * checkpoint calls is a root: it is designed to wait. */
static HashTable phasync_suspend_roots;   /* frames the preempt function was called from */

static bool phasync_str_in(const zend_string *s, const char *const *list, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (zend_string_equals_cstr(s, list[i], strlen(list[i]))) {
			return true;
		}
	}
	return false;
}
#define PHASYNC_STR_IN(s, list) phasync_str_in((s), (list), sizeof(list) / sizeof(*(list)))

/* The C function on the stack: its callbacks never suspend. */
static bool phasync_never_cfunc(const zend_function *fn)
{
	static const char *const funcs[] = {
		"usort", "uasort", "uksort", "serialize", "unserialize", "json_encode", "var_dump", "print_r",
		"debug_zval_dump", "mb_ereg_replace_callback", "spl_autoload_call", "spl_autoload", "gc_destructor_fiber",
		"curl_exec", "curl_multi_exec" };
	static const char *const prefixes[] = { "array_u", "array_diff_u", "array_intersect_u" };
	static const char *const classes[] = { "UConverter", "XSLTProcessor", "DOMXPath", "Dom\\XPath", "ZipArchive" };
	static const char *const modules[] = { "session", "soap", "readline", "ldap" };
	const zend_string *name = fn->common.function_name;
	const zend_class_entry *ce = fn->common.scope;
	const zend_module_entry *m = fn->internal_function.module;

	if (m) {
		for (size_t i = 0; i < sizeof(modules) / sizeof(*modules); i++) {
			if (strcmp(m->name, modules[i]) == 0) {
				return true;
			}
		}
	}
	if (!ce) {
		if (fn->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) {
			return true;   /* an FFI function: its callbacks are called from foreign code */
		}
		for (size_t i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++) {
			if (strncmp(ZSTR_VAL(name), prefixes[i], strlen(prefixes[i])) == 0) {
				return true;
			}
		}
		return PHASYNC_STR_IN(name, funcs);
	}
	return PHASYNC_STR_IN(ce->name, classes)
	 || ((zend_string_equals_literal(ce->name, "ArrayObject") || zend_string_equals_literal(ce->name, "ArrayIterator"))
	  && (zend_string_equals_literal(name, "uasort") || zend_string_equals_literal(name, "uksort")));
}

/* A user stream wrapper's stream_open/stream_read/stream_write, called by the stream layer
 * for one of these plain I/O functions, may wait (#107): userspace.c keeps no lock or
 * global state across that call; only the stream being read is mid-operation, and using
 * one stream from two coroutines at once is the caller's error. */
static bool phasync_stream_op_call(const zend_execute_data *callee, const zend_execute_data *from)
{
	static const char *const ops[] = { "fopen", "fread", "fwrite", "fputs", "fgets", "fgetc",
		"file_get_contents", "file_put_contents", "stream_get_contents", "stream_get_line",
		"fpassthru", "readfile", "file" };
	static const char *const methods[] = { "stream_open", "stream_read", "stream_write" };

	return from && from->func && !ZEND_USER_CODE(from->func->type) && !from->func->common.scope
	 && PHASYNC_STR_IN(from->func->common.function_name, ops)
	 && PHASYNC_STR_IN(callee->func->common.function_name, methods);
}

/* Argument i (0-based) of a user frame; the arguments beyond the declared ones are moved behind the temporaries. */
static const zval *phasync_call_arg(const zend_execute_data *ex, uint32_t i)
{
	const zend_op_array *oa = &ex->func->op_array;

	return i < oa->num_args ? ZEND_CALL_ARG(ex, i + 1) : ZEND_CALL_VAR_NUM(ex, oa->last_var + oa->T) + (i - oa->num_args);
}

/* A user frame the C code (or the engine) entered: a callback that never suspends.
 * prev is the frame it was entered from; caller is that frame when it is user code
 * (an opcode), else NULL. */
static bool phasync_never_callback(const zend_execute_data *ex, const zend_execute_data *prev, const zend_execute_data *caller)
{
	static const char *const methods[] = { "__get", "__set", "__isset", "__unset", "__sleep", "__wakeup",
		"__serialize", "__unserialize", "__debugInfo", "toUCallback", "fromUCallback" };
	static const char *const wrappers[] = { "stream_close", "stream_cast", "stream_eof", "stream_flush",
		"stream_lock", "stream_metadata", "stream_open", "stream_read", "stream_seek", "stream_set_option",
		"stream_stat", "stream_tell", "stream_truncate", "stream_write", "dir_closedir", "dir_opendir",
		"dir_readdir", "dir_rewinddir", "mkdir", "rename", "rmdir", "unlink", "url_stat",
		"filter", "onCreate", "onClose" };
	const zend_function *fn = ex->func;
	const zend_string *name = fn->common.function_name;
	uint32_t argc = ZEND_CALL_NUM_ARGS(ex);

	if (fn == PHASYNC_G(header_cb)) {
		return true;
	}
	if (name && fn->common.scope) {
		if (PHASYNC_STR_IN(name, methods)) {
			return true;
		}
		if (PHASYNC_STR_IN(name, wrappers) && !phasync_stream_op_call(ex, prev)) {
			return true;
		}
	}
	/* An error handler: (int $errno, string $errstr, string $errfile, int $errline). */
	if (argc == 4 && Z_TYPE_P(phasync_call_arg(ex, 0)) == IS_LONG && Z_TYPE_P(phasync_call_arg(ex, 1)) == IS_STRING
	 && Z_TYPE_P(phasync_call_arg(ex, 2)) == IS_STRING && Z_TYPE_P(phasync_call_arg(ex, 3)) == IS_LONG) {
		return true;
	}
	/* A pcntl signal handler: (int $signo, array $siginfo), whose "signo" is $signo (PHP before 8.6 does not block fiber switches there). */
	if (argc == 2 && Z_TYPE_P(phasync_call_arg(ex, 0)) == IS_LONG && Z_TYPE_P(phasync_call_arg(ex, 1)) == IS_ARRAY) {
		const zval *signo = zend_hash_str_find(Z_ARRVAL_P(phasync_call_arg(ex, 1)), ZEND_STRL("signo"));

		if (signo && Z_TYPE_P(signo) == IS_LONG && Z_LVAL_P(signo) == Z_LVAL_P(phasync_call_arg(ex, 0))) {
			return true;
		}
	}
	if (argc >= 1) {
		const zval *a = phasync_call_arg(ex, 0);
#if PHP_VERSION_ID >= 80400
		/* The initializer of a lazy object, called with that object. */
		if (Z_TYPE_P(a) == IS_OBJECT && zend_hash_index_exists(&EG(lazy_objects_store).infos, Z_OBJ_HANDLE_P(a))) {
			return true;
		}
#endif
		/* OpenSSL's reneg_limit_callback, called with the stream from inside SSL_read(). */
		if (Z_TYPE_P(a) == IS_RESOURCE) {
			php_stream *s = zend_fetch_resource2_ex((zval *) a, NULL, php_file_le_stream(), php_file_le_pstream());
			if (s && s->ops && s->ops->label && strcmp(s->ops->label, "tcp_socket/ssl") == 0) {
				return true;
			}
		}
	}
	if (caller && !(ZEND_CALL_INFO(ex) & ZEND_CALL_CODE)) {
		switch (caller->opline->opcode) {
			case ZEND_INCLUDE_OR_EVAL:   /* a stream wrapper or autoloader while the file is opened, an error handler while it compiles */
			/* constant expressions: new in an initializer, a default argument, a class constant, a static */
			case ZEND_RECV_INIT: case ZEND_NEW: case ZEND_BIND_STATIC: case ZEND_DECLARE_CONST:
			case ZEND_FETCH_CONSTANT: case ZEND_FETCH_CLASS_CONSTANT:
			case ZEND_FETCH_STATIC_PROP_R: case ZEND_FETCH_STATIC_PROP_W: case ZEND_FETCH_STATIC_PROP_RW:
			case ZEND_FETCH_STATIC_PROP_IS: case ZEND_FETCH_STATIC_PROP_FUNC_ARG: case ZEND_FETCH_STATIC_PROP_UNSET:
			case ZEND_ASSIGN_STATIC_PROP: case ZEND_ASSIGN_STATIC_PROP_OP: case ZEND_ASSIGN_STATIC_PROP_REF:
			case ZEND_DECLARE_CLASS: case ZEND_DECLARE_CLASS_DELAYED: case ZEND_DECLARE_ANON_CLASS:
				return true;
		}
	}
	return false;
}

#if PHP_VERSION_ID >= 80600
# define PHASYNC_IN_AUTOLOAD() zend_hash_num_elements(&EG(autoload_current_classnames))
#else
# define PHASYNC_IN_AUTOLOAD() (EG(in_autoload) && zend_hash_num_elements(EG(in_autoload)))
#endif

/* Does the stack from ex down forbid suspending? With preempt, ex is the frame of a
 * checkpoint, and a destructor, #[phasync\Uninterruptible] or the bottom of the
 * script's stack (an exception handler, a shutdown function) forbids it too; otherwise
 * ex is the C function doing the blocking operation, which is skipped. */
static bool phasync_never_suspend(const zend_execute_data *ex, bool preempt)
{
	const zend_execute_data *bottom = EG(active_fiber) ? EG(active_fiber)->stack_bottom : NULL;

	if (zend_fiber_switch_blocked() || PHASYNC_IN_AUTOLOAD() || OG(running)) {
		return true;
	}
	if (!preempt && ex && ex->func && !ZEND_USER_CODE(ex->func->type)) {
		ex = ex->prev_execute_data;   /* the operation itself */
	}
	for (const zend_execute_data *prev; ex && ex != bottom; ex = prev) {
		prev = ex->prev_execute_data;
		if (!ex->func) {
			continue;
		}
		if (!ZEND_USER_CODE(ex->func->type)) {
			if (phasync_never_cfunc(ex->func)) {
				return true;
			}
			continue;
		}
		if ((ZEND_CALL_INFO(ex) & ZEND_CALL_TOP) && prev
		 && zend_hash_index_exists(&phasync_suspend_roots, (zend_ulong) (uintptr_t) prev)) {
			return preempt;   /* a wait there is what it is for; it is not re-entrant */
		}
		if (ex->func->op_array.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) {
			continue;
		}
		if (preempt && phasync_op_array_no_preempt(&ex->func->op_array)) {
			return true;
		}
		if ((ZEND_CALL_INFO(ex) & ZEND_CALL_TOP) && !(ZEND_CALL_INFO(ex) & ZEND_CALL_GENERATOR)
		 && phasync_never_callback(ex, prev, prev && prev != bottom && prev->func && ZEND_USER_CODE(prev->func->type) ? prev : NULL)) {
			return true;
		}
		if (preempt && !prev) {
			return ex->func->op_array.function_name != NULL;
		}
	}
	return false;
}

static bool phasync_from_c(void)
{
	return phasync_never_suspend(EG(current_execute_data), false);
}

/* A wait phasync_from_c() refuses: poll(2) the descriptor, blocking. */
static int phasync_wait_inline(php_socket_t fd, int dir, double timeout)
{
	struct pollfd pfd = { .fd = fd, .events = dir == PHASYNC_WRITE ? POLLOUT : POLLIN };
	int ms = isinf(timeout) ? -1 : (int) ceil(timeout * 1000), n;

	do {
		n = poll(&pfd, 1, ms);
	} while (n < 0 && errno == EINTR);
	return n == 0 ? PHASYNC_WAIT_TIMEOUT : PHASYNC_WAIT_READY;
}

/* Wait (in a fiber) until fd is readable or writable, or closed or failed.
 * timeout is the native timeout, INFINITY for none. Returns READY, TIMEOUT or
 * ERROR (exception pending); see phasync_park(). A regular file is always
 * ready: with via_loop (Poller::readable()/writable()) the coroutine still
 * waits for the loop's next poll(), as phasync's own wait on it does without
 * the extension (#29); hooked operations go on at once. */
static int phasync_poller_wait(phasync_poller *p, int dir, php_stream *stream, php_socket_t fd,
                               double timeout, zend_class_entry *timeout_ce, bool via_loop)
{
	int idx = dir == PHASYNC_WRITE, rc, dupfd = -1;
	phasync_reg *r;
	phasync_ledger *l;
	zend_long slot;

	if (!phasync_poller_usable(p)) {
		return PHASYNC_WAIT_ERROR;
	}
	if (fd == -1) {
		/* As stream_select() says it of a stream it cannot cast (the pure-PHP driver). */
		zend_value_error("No stream arrays were passed");
		return PHASYNC_WAIT_ERROR;
	}
	if (phasync_from_c()) {
		return phasync_wait_inline(fd, dir, timeout);
	}
	r = zend_hash_index_find_ptr(&p->regs, (zend_ulong) fd);
	if (r && stream && r->stream && r->stream != stream) {
		if (r->hooked) {
			/* The other stream's close op never deregistered it: a bug in the
			 * extension. Never re-register quietly. */
			zend_error_noreturn(E_ERROR, "phasync: descriptor %d is still registered for stream %p, now waited on by stream %p",
				(int) fd, (void *) r->stream, (void *) stream);
		}
		/* The previous owner had no close hook: it is gone, its descriptor reused. */
		r->stream = stream;
		r->hooked = phasync_has_close_hook(stream);
		r->sid[0] = r->sid[1] = -1;
	}
	if (r && !stream) {
		/* A bare-descriptor wait on a descriptor a stream owns (echo to STDOUT):
		 * wait on a duplicate, which epoll keeps apart. */
		if ((dupfd = dup(fd)) < 0) {
			zend_throw_error(NULL, "Unable to wait on descriptor %d: %s", (int) fd, strerror(errno));
			return PHASYNC_WAIT_ERROR;
		}
		fd = dupfd;
		r = NULL;
	}
	if (r == NULL) {
		r = pecalloc(1, sizeof(*r), 1);
		r->fd = fd;
		r->stream = stream;
		r->slot[0] = r->slot[1] = -1;
		r->sid[0] = r->sid[1] = -1;
		r->transient = stream == NULL;
		r->hooked = stream && phasync_has_close_hook(stream);
		zend_hash_index_add_new_ptr(&p->regs, (zend_ulong) fd, r);
		if (p->select_mode && zend_hash_num_elements(&p->regs) >= PHASYNC_SELECT_THRESHOLD) {
			phasync_poller_use_epoll(p);
		}
	}
	if (r->slot[idx] >= 0) {
		zend_throw_exception_ex(spl_ce_LogicException, 0, "Another coroutine is already waiting to %s this stream",
			idx ? "write to" : "read from");
		rc = PHASYNC_WAIT_ERROR;
		goto done;
	}
	if ((slot = r->hooked && r->sid[idx] >= 0 ? r->sid[idx] : phasync_get_slot(p)) < 0) {
		rc = PHASYNC_WAIT_ERROR;
		goto done;
	}
	if (r->hooked) {
		r->sid[idx] = slot;
	}
	r->slot[idx] = slot;
	p->armed++;
	if (phasync_reg_arm(p, r) != 0) {
		r->slot[idx] = -1;
		p->armed--;
		if (errno == EPERM && via_loop) {
			phasync_chan_push(p->chan, slot);   /* a regular file: ready at the next poll() */
			goto park;
		} else if (errno == EPERM) {
			rc = PHASYNC_WAIT_READY;   /* a regular file: always ready, as select() says */
		} else {
			zend_throw_error(NULL, "Unable to wait on descriptor %d: %s", (int) fd, strerror(errno));
			rc = PHASYNC_WAIT_ERROR;
		}
		goto done;
	}

park:
	GC_ADDREF(&p->std);                  /* the loop may drop the Poller while we wait */
	l = phasync_ledger_cur();
	l->wait_p = p;
	l->wait_stream = stream;
	l->wait_fd = fd;
	l->wait_idx = idx;
	l->wait_dupfd = dupfd;
	l->wait_slot = slot;
	rc = phasync_park(p, slot, timeout, timeout_ce);
	l->wait_p = NULL;
	phasync_wait_settle(p, idx, stream, fd, slot, dupfd);
	return rc;
done:
	if (r && r->transient && r->slot[0] < 0 && r->slot[1] < 0) {
		phasync_reg_drop(p, r);
	}
	if (dupfd >= 0) {
		close(dupfd);
	}
	return rc;
}

/* ---- phasync\ext\Poller -------------------------------------------------- */

static zend_object *phasync_poller_create(zend_class_entry *ce)
{
	phasync_poller *p = zend_object_alloc(sizeof(phasync_poller), ce);

	zend_object_std_init(&p->std, ce);
	object_properties_init(&p->std, ce);
	p->std.handlers = &phasync_poller_handlers;
	p->epfd = -1;
	p->fork_gen = phasync_fork_gen;
	p->select_mode = true;   /* 0 watched fds: starts below the threshold */
	ZVAL_UNDEF(&p->get_slot);
	ZVAL_UNDEF(&p->park);
	ZVAL_UNDEF(&p->unpark);
	zend_hash_init(&p->regs, 16, NULL, phasync_reg_dtor, 1);
	p->next = phasync_pollers;
	phasync_pollers = p;
	return &p->std;
}

/* Waiters still parked through a freed Poller are never unparked: their waits
 * run to their timeouts (a waiter keeps the Poller alive while it waits, so this
 * happens only to the loop's own parked coroutines). In a fork()ed child only
 * the child's copies of the descriptors are closed. */
static void phasync_poller_free(zend_object *obj)
{
	phasync_poller *p = phasync_poller_from(obj);

	for (phasync_poller **pp = &phasync_pollers; *pp; pp = &(*pp)->next) {
		if (*pp == p) {
			*pp = p->next;
			break;
		}
	}
	zend_hash_destroy(&p->regs);         /* closing the epoll set drops the registrations */
	phasync_backend_destroy(p);
	free(p->sel_rfds);
	free(p->sel_wfds);
	if (p->chan) {
		if (p->fork_gen == phasync_fork_gen) {
			phasync_chan_release(p->chan);
		} else {
			phasync_chan_close(p->chan); /* its mutex may be the parent's, held at fork */
		}
	}
	zval_ptr_dtor(&p->get_slot);
	zval_ptr_dtor(&p->park);
	zval_ptr_dtor(&p->unpark);
	zend_object_std_dtor(obj);
}

/* The closures usually capture the loop that owns the Poller: let the cycle
 * collector see them. */
static HashTable *phasync_poller_get_gc(zend_object *obj, zval **table, int *n)
{
	phasync_poller *p = phasync_poller_from(obj);

	*table = &p->get_slot;               /* get_slot, park, unpark are adjacent */
	*n = 3;
	return zend_std_get_properties(obj);
}

ZEND_METHOD(phasync_ext_Poller, __construct)
{
	zval *get_slot, *park, *unpark;
	phasync_poller *p = phasync_poller_from(Z_OBJ_P(ZEND_THIS));
	phasync_chan *ch;
	bool chan_added = false;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_OBJECT_OF_CLASS(get_slot, zend_ce_closure)
		Z_PARAM_OBJECT_OF_CLASS(park, zend_ce_closure)
		Z_PARAM_OBJECT_OF_CLASS(unpark, zend_ce_closure)
	ZEND_PARSE_PARAMETERS_END();

	if (p->epfd >= 0) {
		zend_throw_error(NULL, "The Poller is already constructed");
		RETURN_THROWS();
	}
	ch = calloc(1, sizeof(*ch));
	phasync_backend_create(p);
	if (ch) {
		phasync_chan_open(ch);
	}
	if (!ch || p->epfd < 0 || ch->evfd < 0) {
		int err = errno;
		if (ch) {
			if (ch->evfd >= 0) phasync_chan_close(ch);
			free(ch);
		}
		zend_throw_error(NULL, "Unable to create the epoll set: %s", strerror(err));
		RETURN_THROWS();
	}
	pthread_mutex_init(&ch->mutex, NULL);
	ch->refs = 1;
	p->chan = ch;
	phasync_backend_set(p, ch->evfd, PHASYNC_EV_IN, &chan_added);   /* its own fd, never a reg's: see poll() */
	ZVAL_COPY(&p->get_slot, get_slot);
	ZVAL_COPY(&p->park, park);
	ZVAL_COPY(&p->unpark, unpark);
}

static phasync_poller *phasync_this_poller(zval *this_zv)
{
	phasync_poller *p = phasync_poller_from(Z_OBJ_P(this_zv));

	if (p->epfd < 0) {
		zend_throw_error(NULL, "The Poller is not constructed");
		return NULL;
	}
	return phasync_poller_usable(p) ? p : NULL;
}

/* The event loop's one blocking call: wait up to $maxTime seconds (0: don't
 * wait) in one epoll_wait(), then unpark the waiters of what became ready, and
 * of the hooked operations started under a manage() given this Poller whose
 * thread tasks finished. Costs per ready event, never a scan of everything
 * registered. */
/* Grow *wake (from wake_stack to heap, or heap to bigger heap) so it holds at
 * least nwake + extra entries. Shared by the epoll and select wait paths. */
static void phasync_wake_reserve(zend_long **wake, zend_long *wake_stack, size_t *cap, size_t nwake, size_t extra)
{
	size_t newcap;

	if (nwake + extra <= *cap) {
		return;
	}
	newcap = *cap * 2 > nwake + extra ? *cap * 2 : nwake + extra;
	*wake = *wake == wake_stack ? memcpy(emalloc(newcap * sizeof(**wake)), wake_stack, nwake * sizeof(**wake))
	                            : erealloc(*wake, newcap * sizeof(**wake));
	*cap = newcap;
}

/* epoll_wait() and settle every registration it reports, building *wake.
 * Returns 0 (built, caller unparks), 1 (EINTR or nothing to do: return), or
 * -1 (exception thrown). */
static int phasync_poller_epoll_wait(phasync_poller *p, double max_time, zend_long **wake, zend_long *wake_stack,
                                      size_t *nwake, size_t *cap)
{
	phasync_event ev[256];
	/* Round up, so a wait shorter than a millisecond doesn't spin. */
	int ms = max_time <= 0 ? 0 : (max_time >= INT_MAX / 1000 ? -1 : (int) ceil(max_time * 1000));
	int n = phasync_backend_wait(p, ev, sizeof(ev) / sizeof(ev[0]), ms);

	if (n < 0) {
		if (errno == EINTR) {
			return 1;                        /* a signal: its handler has run */
		}
		zend_throw_error(NULL, "epoll_wait() failed: %s", strerror(errno));
		return -1;
	}
	for (int i = 0; i < n; i++) {
		if ((php_socket_t) ev[i].data.u64 == p->chan->evfd) {
			phasync_chan *ch = p->chan;
			phasync_chan_drain(ch);
			pthread_mutex_lock(&ch->mutex);
			phasync_wake_reserve(wake, wake_stack, cap, *nwake, ch->len);
			memcpy(*wake + *nwake, ch->queue, ch->len * sizeof(**wake));
			*nwake += ch->len;
			ch->len = 0;
			pthread_mutex_unlock(&ch->mutex);
		} else {
			phasync_reg *r = zend_hash_index_find_ptr(&p->regs, (zend_ulong) ev[i].data.u64);
			uint32_t got = ev[i].events;
			bool rd, wr;

			if (r == NULL) {
				continue;
			}
			/* Ready also when closed or failed: the op then reports it as PHP does. */
			rd = PHASYNC_REG_WAITS(r, 0) && (got & (PHASYNC_EV_IN | PHASYNC_EV_RDHUP | PHASYNC_EV_HUP | PHASYNC_EV_ERR));
			wr = PHASYNC_REG_WAITS(r, 1) && (got & (PHASYNC_EV_OUT | PHASYNC_EV_HUP | PHASYNC_EV_ERR));
			if (r->hooked) {
				/* Level-triggered: disarm what fired with nobody waiting for it. */
				uint32_t stray = (!PHASYNC_REG_WAITS(r, 0) && (got & (PHASYNC_EV_IN | PHASYNC_EV_RDHUP)) ? PHASYNC_EV_IN | PHASYNC_EV_RDHUP : 0)
				               | (!PHASYNC_REG_WAITS(r, 1) && (got & PHASYNC_EV_OUT) ? PHASYNC_EV_OUT : 0);
				if ((got & (PHASYNC_EV_HUP | PHASYNC_EV_ERR)) && !rd && !wr) {
					phasync_backend_remove(p, r->fd, &r->added);
					r->events = 0;
				} else if (stray & r->events) {
					if ((r->events & ~stray) == 0) {
						phasync_backend_remove(p, r->fd, &r->added);
						r->events = 0;
					} else {
						phasync_reg_ctl(p, r, r->events & ~stray, false);
					}
				}
			}
			phasync_wake_reserve(wake, wake_stack, cap, *nwake, 2);
			if (rd) {
				(*wake)[(*nwake)++] = r->slot[0];
				r->slot[0] = -1;
				p->armed--;
			}
			if (wr) {
				(*wake)[(*nwake)++] = r->slot[1];
				r->slot[1] = -1;
				p->armed--;
			}
			if (!r->hooked && (PHASYNC_REG_WAITS(r, 0) || PHASYNC_REG_WAITS(r, 1))) {
				phasync_reg_arm(p, r);       /* one-shot: the other direction still waits */
			}
		}
	}
	return 0;
}

/* select()'s equivalent of phasync_poller_epoll_wait(): the read/write sets are
 * rebuilt from p->regs on every call (no registration kept between calls), in
 * buffers grown past FD_SETSIZE as needed (PHASYNC_FD_SET/ISSET, not libc's
 * fortified ones). Regular files need no special case: unlike epoll_ctl(),
 * select() always reports them ready. */
static int phasync_poller_select_wait(phasync_poller *p, double max_time, zend_long **wake, zend_long *wake_stack,
                                       size_t *nwake, size_t *cap)
{
	phasync_reg *r;
	php_socket_t maxfd = p->chan->evfd;
	size_t need;
	struct timeval tv, *tvp;
	bool bad;
	int n;

	ZEND_HASH_FOREACH_PTR(&p->regs, r) {
		if ((PHASYNC_REG_WAITS(r, 0) || PHASYNC_REG_WAITS(r, 1)) && r->fd > maxfd) {
			maxfd = r->fd;
		}
	} ZEND_HASH_FOREACH_END();

	need = PHASYNC_FDSET_BYTES(maxfd);
	if (need > p->sel_cap) {
		void *rfds = realloc(p->sel_rfds, need);
		void *wfds;
		if (rfds == NULL) {
			zend_throw_error(NULL, "Unable to allocate select() descriptor sets: %s", strerror(errno));
			return -1;
		}
		p->sel_rfds = rfds;
		wfds = realloc(p->sel_wfds, need);
		if (wfds == NULL) {
			zend_throw_error(NULL, "Unable to allocate select() descriptor sets: %s", strerror(errno));
			return -1;
		}
		p->sel_wfds = wfds;
		p->sel_cap = need;
	}
	memset(p->sel_rfds, 0, need);
	memset(p->sel_wfds, 0, need);
	PHASYNC_FD_SET(p->chan->evfd, p->sel_rfds);
	ZEND_HASH_FOREACH_PTR(&p->regs, r) {
		if (PHASYNC_REG_WAITS(r, 0)) {
			PHASYNC_FD_SET(r->fd, p->sel_rfds);
		}
		if (PHASYNC_REG_WAITS(r, 1)) {
			PHASYNC_FD_SET(r->fd, p->sel_wfds);
		}
	} ZEND_HASH_FOREACH_END();

	if (max_time <= 0) {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		tvp = &tv;
	} else if (max_time >= INT_MAX / 1000) {
		tvp = NULL;                          /* effectively forever, as epoll_wait's ms=-1 */
	} else {
		tv.tv_sec = (time_t) max_time;
		tv.tv_usec = (long) ((max_time - tv.tv_sec) * 1e6 + 0.5);
		tvp = &tv;
	}
	n = select(maxfd + 1, (fd_set *) p->sel_rfds, (fd_set *) p->sel_wfds, NULL, tvp);
	if (n < 0 && errno == EINTR) {
		return 1;                            /* a signal: its handler has run */
	}
	if (n < 0 && errno != EBADF) {
		zend_throw_error(NULL, "select() failed: %s", strerror(errno));
		return -1;
	}
	/* EBADF: a waited descriptor was closed past our close hooks (epoll forgets
	 * it silently). Wake its waiters below; their op finds it closed. */
	bad = n < 0;
	if (!bad && PHASYNC_FD_ISSET(p->chan->evfd, p->sel_rfds)) {
		phasync_chan *ch = p->chan;
		phasync_chan_drain(ch);
		pthread_mutex_lock(&ch->mutex);
		phasync_wake_reserve(wake, wake_stack, cap, *nwake, ch->len);
		memcpy(*wake + *nwake, ch->queue, ch->len * sizeof(**wake));
		*nwake += ch->len;
		ch->len = 0;
		pthread_mutex_unlock(&ch->mutex);
	}
	ZEND_HASH_FOREACH_PTR(&p->regs, r) {
		bool closed = bad && fcntl(r->fd, F_GETFD) < 0;
		bool rd = PHASYNC_REG_WAITS(r, 0) && (bad ? closed : PHASYNC_FD_ISSET(r->fd, p->sel_rfds));
		bool wr = PHASYNC_REG_WAITS(r, 1) && (bad ? closed : PHASYNC_FD_ISSET(r->fd, p->sel_wfds));
		if (!rd && !wr) {
			continue;
		}
		phasync_wake_reserve(wake, wake_stack, cap, *nwake, 2);
		if (rd) {
			(*wake)[(*nwake)++] = r->slot[0];
			r->slot[0] = -1;
			p->armed--;
		}
		if (wr) {
			(*wake)[(*nwake)++] = r->slot[1];
			r->slot[1] = -1;
			p->armed--;
		}
	} ZEND_HASH_FOREACH_END();
	return 0;
}

ZEND_METHOD(phasync_ext_Poller, poll)
{
	double max_time;
	zend_long wake_stack[512], *wake = wake_stack;
	size_t nwake = 0, cap = sizeof(wake_stack) / sizeof(wake_stack[0]);
	phasync_poller *p;
	int rc;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_DOUBLE(max_time)
	ZEND_PARSE_PARAMETERS_END();

	if ((p = phasync_this_poller(ZEND_THIS)) == NULL) {
		RETURN_THROWS();
	}
	phasync_holds_release();
	if (EG(exception)) {
		RETURN_THROWS();
	}
	if (max_time <= 0 && p->armed == 0) {
		/* The loop polls without waiting on every tick while it has work: with no
		 * descriptor waited on and no finished thread task queued, skip the syscall. */
		size_t queued;
		pthread_mutex_lock(&p->chan->mutex);
		queued = p->chan->len;
		pthread_mutex_unlock(&p->chan->mutex);
		if (queued == 0) {
			return;
		}
	}
	rc = p->select_mode ? phasync_poller_select_wait(p, max_time, &wake, wake_stack, &nwake, &cap)
	                     : phasync_poller_epoll_wait(p, max_time, &wake, wake_stack, &nwake, &cap);
	if (rc != 0) {
		if (wake != wake_stack) {
			efree(wake);
		}
		if (rc < 0) {
			RETURN_THROWS();
		}
		return;
	}
	for (size_t i = 0; i < nwake && !EG(exception); i++) {
		zval arg, retval;
		ZVAL_LONG(&arg, wake[i]);
		ZVAL_UNDEF(&retval);
		call_user_function(NULL, NULL, &p->unpark, &retval, 1, &arg);
		zval_ptr_dtor(&retval);
	}
	if (wake != wake_stack) {
		efree(wake);
	}
}

static void phasync_poller_wait_method(INTERNAL_FUNCTION_PARAMETERS, int dir)
{
	zval *zstream;
	double timeout = DBL_MAX;
	php_stream *stream;
	phasync_poller *p;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_RESOURCE(zstream)
		Z_PARAM_OPTIONAL
		Z_PARAM_DOUBLE(timeout)
	ZEND_PARSE_PARAMETERS_END();

	php_stream_from_zval(stream, zstream);
	if ((p = phasync_this_poller(ZEND_THIS)) == NULL) {
		RETURN_THROWS();
	}
	if (EG(active_fiber) == NULL) {
		zend_throw_error(NULL, "Poller::%s() must be called in a coroutine", dir == PHASYNC_READ ? "readable" : "writable");
		RETURN_THROWS();
	}
	/* The loop's exceptions (a timeout, a cancellation) reach the caller as they are. */
	phasync_poller_wait(p, dir, stream, phasync_stream_fd(stream), timeout >= DBL_MAX ? INFINITY : timeout, NULL, true);
}

ZEND_METHOD(phasync_ext_Poller, readable)
{
	phasync_poller_wait_method(INTERNAL_FUNCTION_PARAM_PASSTHRU, PHASYNC_READ);
}

ZEND_METHOD(phasync_ext_Poller, writable)
{
	phasync_poller_wait_method(INTERNAL_FUNCTION_PARAM_PASSTHRU, PHASYNC_WRITE);
}

