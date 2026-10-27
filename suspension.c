/* Included by phasync.c (single translation unit); not compiled on its own. */

/* phasync\ext\Suspension: how a blocking call in a managed fiber reaches the event loop.
 *
 * A fiber opts in with Suspension::manage($fiber). When code in it (or in an unmanaged
 * fiber it runs) makes a hooked blocking call, the extension suspends the nearest managed
 * fiber and throws a Suspension out of the resume()/start()/throw() that ran it. The loop
 * collects them, polls them with Suspension::poll(), and resumes the ones that are ready;
 * the blocked call then completes in the fiber as it would have natively.
 *
 * Suspensions are pooled: a managed fiber has at most one pending at a time, so the pool
 * grows to the number of fibers suspended at once and stays there. While a wait is on, the
 * loop owns the Suspension (it was thrown to it) and the Suspension owns the fiber; a loop
 * that drops a pending one drops its fiber too, which is then destroyed as PHP destroys
 * any suspended fiber nobody holds. One whose wait is over goes back into use once only the
 * pool holds it; WeakMap entries and WeakReferences to it are cleared then, as if it had
 * been freed. They are never buffered as GC roots: a
 * Suspension holds no PHP values (final, no dynamic properties, Exception's previous and
 * trace stay empty), so it can't take part in a cycle.
 *
 * Each Suspension's index in the pool is the slot the waiting code (poller.c, io.c,
 * pool.c) parks in: phasync_get_slot() takes one, phasync_park() suspends with it, and
 * Suspension::poll() marks the slots that became ready. */

#include "Zend/zend_weakrefs.h"

#define PHASYNC_S_PENDING 1   /* waiting for its descriptor, timer or thread task */
#define PHASYNC_S_READY   2   /* the fiber can be resumed */
#define PHASYNC_S_RETIRED 3   /* its wait is over; reused once only the pool holds it */

typedef struct phasync_susp {
	uint32_t     idx;
	uint8_t      state;
	bool         timed_out;   /* READY because its deadline passed */
	bool         owned;       /* the pool holds a reference (spare, or dropped by the loop) */
	double       deadline;    /* phasync_now() clock; INFINITY: none */
	zend_fiber  *fiber;       /* the managed fiber it suspended (a reference) */
	zval         members;     /* a poll() of these, for an outer loop; UNDEF otherwise */
	zend_object  std;
} phasync_susp;

/* zend_fiber.flags bit: Suspension::manage() was given this fiber. The engine uses the
 * three lowest bits (8.2-8.5). */
#define PHASYNC_FIBER_MANAGED 0x80

static zend_class_entry *phasync_suspension_ce;
static zend_object_handlers phasync_suspension_handlers;

static zend_always_inline phasync_susp *phasync_susp_from(zend_object *obj)
{
	return (phasync_susp *) ((char *) obj - offsetof(phasync_susp, std));
}

/* The fiber to suspend for a blocking call made now: the running fiber if it is
 * managed, else the nearest managed fiber it runs under. NULL: none, or a fiber switch
 * isn't possible here; the call then blocks natively. */
static zend_fiber *phasync_managed_top(void)
{
	zend_fiber *f = EG(active_fiber);

	if (f == NULL || zend_fiber_switch_blocked()) {
		return NULL;
	}
	while (!(f->flags & PHASYNC_FIBER_MANAGED)) {
		zend_fiber_context *caller = f->caller;
		if (caller == NULL || caller->kind != zend_ce_fiber) {
			return NULL;
		}
		f = (zend_fiber *) ((char *) caller - offsetof(zend_fiber, context));
	}
	return f;
}

/* The hooked operations' "scope": one per request, present while code runs under a
 * managed fiber. */
static phasync_poller *phasync_proc_poller(void);

static phasync_scope *phasync_scope_top(void)
{
	if (phasync_managed_top() == NULL) {
		return NULL;
	}
	PHASYNC_G(proc_scope).poller = phasync_proc_poller();
	return PHASYNC_G(proc_scope).poller ? &PHASYNC_G(proc_scope) : NULL;
}

/* ---- the pool ---------------------------------------------------------------- */

static zend_object *phasync_suspension_create(zend_class_entry *ce)
{
	phasync_susp *s = zend_object_alloc(sizeof(phasync_susp), ce);

	zend_object_std_init(&s->std, ce);
	object_properties_init(&s->std, ce);
	s->std.handlers = &phasync_suspension_handlers;
	ZVAL_UNDEF(&s->members);
	s->owned = true;
	GC_ADD_FLAGS(&s->std, GC_NOT_COLLECTABLE);
	return &s->std;
}

/* The loop let go of a Suspension it was given: the pool takes it back. Its fiber, if
 * still waiting, is released later (not from inside a destructor, where fibers can't
 * switch): destroyed then unless someone else holds it. */
static void phasync_suspension_dtor(zend_object *obj)
{
	phasync_susp *s = phasync_susp_from(obj);

	if (s->owned) {
		return;
	}
	GC_ADDREF(obj);
	GC_DEL_FLAGS(obj, IS_OBJ_DESTRUCTOR_CALLED);   /* so it comes here again next time */
	s->owned = true;
	if (s->fiber) {
		if (PHASYNC_G(norphans) == PHASYNC_G(caporphans)) {
			PHASYNC_G(caporphans) = PHASYNC_G(caporphans) ? PHASYNC_G(caporphans) * 2 : 16;
			PHASYNC_G(orphans) = erealloc(PHASYNC_G(orphans), PHASYNC_G(caporphans) * sizeof(zend_fiber *));
		}
		PHASYNC_G(orphans)[PHASYNC_G(norphans)++] = s->fiber;
		s->fiber = NULL;
	}
}

/* Release the fibers of Suspensions the loop dropped. */
static void phasync_orphans_release(void)
{
	while (PHASYNC_G(norphans)) {
		zend_fiber *f = PHASYNC_G(orphans)[--PHASYNC_G(norphans)];
		OBJ_RELEASE(&f->std);
	}
}

static void phasync_suspension_free(zend_object *obj)
{
	phasync_susp *s = phasync_susp_from(obj);

	zval_ptr_dtor(&s->members);
	if (s->fiber) {
		OBJ_RELEASE(&s->fiber->std);
	}
	zend_object_std_dtor(obj);
}

/* A Suspension for a new wait, from the pool or new. */
static phasync_susp *phasync_susp_take(void)
{
	uint32_t *spare = PHASYNC_G(susp_spare);
	phasync_susp *s;

	/* The most recently retired are the likeliest to be held still (the loop is about to
	 * resume them), so look from the oldest; a few tries bound the cost. */
	for (uint32_t i = 0, tries = 0; i < PHASYNC_G(nspare) && tries < 8; i++, tries++) {
		s = PHASYNC_G(susp)[spare[i]];
		if (GC_REFCOUNT(&s->std) == 1) {
			spare[i] = spare[--PHASYNC_G(nspare)];
			if (GC_FLAGS(&s->std) & IS_OBJ_WEAKLY_REFERENCED) {
				zend_weakrefs_notify(&s->std);
				GC_DEL_FLAGS(&s->std, IS_OBJ_WEAKLY_REFERENCED);
			}
#if ZEND_DEBUG
			{
				zval *prev = zend_read_property_ex(zend_ce_exception, &s->std, ZSTR_KNOWN(ZEND_STR_PREVIOUS), true, NULL);
				ZEND_ASSERT(prev == NULL || Z_TYPE_P(prev) == IS_NULL);
			}
#endif
			goto ready;
		}
	}
	if (PHASYNC_G(nsusp) == PHASYNC_G(capsusp)) {
		uint32_t cap = PHASYNC_G(capsusp) ? PHASYNC_G(capsusp) * 2 : 64;
		PHASYNC_G(susp) = erealloc(PHASYNC_G(susp), cap * sizeof(phasync_susp *));
		PHASYNC_G(susp_spare) = erealloc(PHASYNC_G(susp_spare), cap * sizeof(uint32_t));
		PHASYNC_G(capsusp) = cap;
	}
	s = phasync_susp_from(phasync_suspension_create(phasync_suspension_ce));
	s->idx = PHASYNC_G(nsusp)++;
	PHASYNC_G(susp)[s->idx] = s;
ready:
	s->state = PHASYNC_S_PENDING;
	s->timed_out = false;
	s->deadline = INFINITY;
	return s;
}

/* Its wait is over: back to the pool, once the loop lets go of it. */
static void phasync_susp_retire(phasync_susp *s)
{
	if (s->state == PHASYNC_S_RETIRED) {
		return;
	}
	s->state = PHASYNC_S_RETIRED;
	if (!s->owned) {
		GC_ADDREF(&s->std);
		s->owned = true;
	}
	if (s->fiber) {
		zend_fiber *f = s->fiber;
		s->fiber = NULL;
		OBJ_RELEASE(&f->std);
	}
	zval_ptr_dtor(&s->members);
	ZVAL_UNDEF(&s->members);
	PHASYNC_G(susp_spare)[PHASYNC_G(nspare)++] = s->idx;
}

static zend_long phasync_get_slot(phasync_poller *p)
{
	(void) p;
	return (zend_long) phasync_susp_take()->idx;
}

/* Called for each slot a poll found ready (an event, or a finished thread task). */
static bool phasync_unpark(zend_long slot)
{
	phasync_susp *s;

	if (slot < 0 || (zend_ulong) slot >= PHASYNC_G(nsusp)) {
		return false;
	}
	s = PHASYNC_G(susp)[slot];
	if (s->state != PHASYNC_S_PENDING) {
		return false;
	}
	s->state = PHASYNC_S_READY;
	return true;
}

/* ---- suspending ---------------------------------------------------------------- */

/* Suspend managed fiber m (the running fiber, or one it runs under) and throw s out of
 * the call that resumed m. When an unmanaged fiber runs under m, the whole chain stays
 * where it is: m looks suspended, the fibers in between look running (their resume()
 * throws FiberError), and resuming m continues the innermost one here. Returns when m is
 * resumed, with the value it was resumed with in *value; true if it was thrown into
 * (Fiber::throw(), or the fiber being destroyed), the Throwable then in *value. */
static bool phasync_susp_switch(zend_fiber *m, phasync_susp *s, zval *value)
{
	zend_fiber *cur = EG(active_fiber);
	zend_fiber_transfer t = { .context = m->caller, .flags = ZEND_FIBER_TRANSFER_FLAG_ERROR };

	if (s->owned) {
		ZVAL_OBJ(&t.value, &s->std);           /* the pool's reference goes to the loop */
		s->owned = false;
	} else {
		ZVAL_OBJ_COPY(&t.value, &s->std);
	}
	if (cur == m) {
		m->execute_data = EG(current_execute_data);
	}
	/* else m->execute_data is its frame calling resume(), saved when it made that call */
	m->stack_bottom->prev_execute_data = NULL;
	m->previous = EG(current_fiber_context);
	m->caller = NULL;

	zend_fiber_switch_context(&t);

	if (UNEXPECTED(t.flags & ZEND_FIBER_TRANSFER_FLAG_BAILOUT)) {
		EG(active_fiber) = NULL;
		zend_bailout();
	}
	EG(active_fiber) = cur;
	ZVAL_COPY_VALUE(value, &t.value);
	return (t.flags & ZEND_FIBER_TRANSFER_FLAG_ERROR) != 0;
}

/* Suspend until s is ready. READY; TIMEOUT once its deadline has passed; ERROR with the
 * exception pending if the loop threw into the fiber (a cancellation) or destroys it. A
 * resume() before it is ready suspends again with the same Suspension. */
static int phasync_susp_wait(zend_fiber *m, phasync_susp *s)
{
	zval v;

	s->fiber = m;
	GC_ADDREF(&m->std);
	for (;;) {
		if (phasync_susp_switch(m, s, &v)) {
			zend_throw_exception_internal(Z_OBJ(v));
			return PHASYNC_WAIT_ERROR;
		}
		zval_ptr_dtor(&v);
		if (s->state == PHASYNC_S_READY) {
			return s->timed_out ? PHASYNC_WAIT_TIMEOUT : PHASYNC_WAIT_READY;
		}
	}
}

/* Park the current coroutine in slot until it is ready, or for at most timeout seconds
 * (INFINITY: no limit): TIMEOUT then, which a hooked call finishes the way PHP does on a
 * native timeout. ERROR with the exception pending on a cancellation. */
static int phasync_park(phasync_poller *p, zend_long slot, double timeout, zend_class_entry *timeout_ce)
{
	phasync_susp *s = PHASYNC_G(susp)[slot];
	zend_fiber *m;
	int rc;

	(void) timeout_ce;
	if (phasync_cannot_suspend()) {
		rc = PHASYNC_WAIT_ERROR;
	} else if ((m = phasync_managed_top()) == NULL) {
		zend_throw_error(NULL, "phasync: a hooked call tried to suspend outside a managed fiber");
		rc = PHASYNC_WAIT_ERROR;
	} else {
		s->deadline = isinf(timeout) ? INFINITY : phasync_now() + timeout;
		rc = phasync_susp_wait(m, s);
	}
	if (rc != PHASYNC_WAIT_READY) {
		phasync_chan_purge(p->chan, slot);
	}
	phasync_susp_retire(s);
	return rc;
}

/* sleep() and kin in a managed fiber: a Suspension with only a deadline. -1 if it threw. */
static int phasync_susp_sleep(double seconds)
{
	phasync_poller *p = phasync_proc_poller();
	zend_long slot;

	if (p == NULL || (slot = phasync_get_slot(p)) < 0) {
		return -1;
	}
	return phasync_park(p, slot, seconds > 0 ? seconds : 0, NULL) == PHASYNC_WAIT_ERROR ? -1 : 0;
}

/* ---- the process's poller --------------------------------------------------------- */

static bool phasync_poller_init(phasync_poller *p);

/* One per request (and per process after a fork()), created on first use. */
static phasync_poller *phasync_proc_poller(void)
{
	phasync_poller *p = PHASYNC_G(proc_poller);

	if (p && p->fork_gen == phasync_fork_gen) {
		return p;
	}
	if (p) {
		PHASYNC_G(proc_poller) = NULL;
		OBJ_RELEASE(&p->std);
	}
	p = phasync_poller_from(phasync_poller_create(phasync_poller_ce));
	if (!phasync_poller_init(p)) {
		OBJ_RELEASE(&p->std);
		return NULL;
	}
	PHASYNC_G(proc_poller) = p;
	return p;
}

/* ---- Suspension::poll() -------------------------------------------------------------- */

/* Mark what timed out among these (and the members of nested polls) ready; the earliest
 * remaining deadline into *next. True if any of them is ready. */
static bool phasync_susp_scan(HashTable *ht, double now, double *next, int depth)
{
	zval *zv;
	bool any = false;

	ZEND_HASH_FOREACH_VAL(ht, zv) {
		phasync_susp *s = phasync_susp_from(Z_OBJ_P(zv));
		if (s->state == PHASYNC_S_PENDING) {
			if (s->deadline <= now) {
				s->state = PHASYNC_S_READY;
				s->timed_out = true;
			} else {
				if (s->deadline < *next) {
					*next = s->deadline;
				}
				if (!Z_ISUNDEF(s->members) && depth < 32
				 && phasync_susp_scan(Z_ARRVAL(s->members), now, next, depth + 1)) {
					s->state = PHASYNC_S_READY;   /* a nested poll has something ready */
				}
			}
		}
		any = any || s->state == PHASYNC_S_READY;
	} ZEND_HASH_FOREACH_END();
	return any;
}

/* Wait at most max_time seconds for events, and mark the slots they wake ready. -1 if it
 * threw. */
static int phasync_poller_pump(phasync_poller *p, double max_time)
{
	zend_long wake_stack[512], *wake = wake_stack;
	size_t nwake = 0, cap = sizeof(wake_stack) / sizeof(wake_stack[0]);
	int rc;

	if (max_time <= 0 && p->armed == 0) {
		/* Polling without waiting on every tick of a busy loop: with no descriptor waited
		 * on and no finished thread task queued, skip the syscall. */
		size_t queued;
		pthread_mutex_lock(&p->chan->mutex);
		queued = p->chan->len;
		pthread_mutex_unlock(&p->chan->mutex);
		if (queued == 0) {
			return 0;
		}
	}
	rc = p->select_mode ? phasync_poller_select_wait(p, max_time, &wake, wake_stack, &nwake, &cap)
	                     : phasync_poller_epoll_wait(p, max_time, &wake, wake_stack, &nwake, &cap);
	if (rc == 0) {
		for (size_t i = 0; i < nwake; i++) {
			phasync_unpark(wake[i]);
		}
	}
	if (wake != wake_stack) {
		efree(wake);
	}
	return rc < 0 ? -1 : 0;
}

ZEND_METHOD(phasync_ext_Suspension, poll)
{
	HashTable *ht;
	double seconds, now, end, next;
	phasync_poller *p;
	zend_string *key;
	zend_ulong h;
	zval *zv;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_ARRAY_HT(ht)
		Z_PARAM_DOUBLE(seconds)
	ZEND_PARSE_PARAMETERS_END();

	ZEND_HASH_FOREACH_VAL(ht, zv) {
		if (Z_TYPE_P(zv) != IS_OBJECT || Z_OBJCE_P(zv) != phasync_suspension_ce) {
			zend_argument_type_error(1, "must contain only phasync\\ext\\Suspension objects, %s given", zend_zval_value_name(zv));
			RETURN_THROWS();
		}
	} ZEND_HASH_FOREACH_END();

	array_init(return_value);
	phasync_orphans_release();
	if (zend_hash_num_elements(ht) == 0 || EG(exception)) {
		return;
	}
	if ((p = phasync_proc_poller()) == NULL) {
		zend_throw_error(NULL, "Unable to create the epoll set: %s", strerror(errno));
		RETURN_THROWS();
	}
	phasync_holds_release();
	if (EG(exception)) {
		RETURN_THROWS();
	}
	now = phasync_now();
	end = seconds < 0 ? INFINITY : now + seconds;
	next = end;
	while (!phasync_susp_scan(ht, now, &next, 0)) {
		zend_fiber *m;

		if (seconds == 0) {
			/* Don't wait: take what the kernel has ready now, and report that. */
			if (phasync_poller_pump(p, 0) < 0) {
				RETURN_THROWS();
			}
			next = end;
			phasync_susp_scan(ht, phasync_now(), &next, 0);
			break;
		}
		if (now >= end) {
			break;
		}
		if ((m = phasync_managed_top()) != NULL) {
			/* A blocking call like any other: suspend the managed fiber for its loop, with a
			 * Suspension that is ready when one of these is, or at the end of $seconds. */
			phasync_susp *s = phasync_susp_take();
			int rc;
			s->deadline = end;
			ZVAL_ARR(&s->members, zend_array_dup(ht));
			rc = phasync_susp_wait(m, s);
			phasync_susp_retire(s);
			if (rc == PHASYNC_WAIT_ERROR) {
				RETURN_THROWS();
			}
		} else if (phasync_poller_pump(p, isinf(next) ? 1e12 : next - now) < 0) {
			RETURN_THROWS();
		}
		now = phasync_now();
		next = end;
	}
	ZEND_HASH_FOREACH_KEY_VAL(ht, h, key, zv) {
		if (phasync_susp_from(Z_OBJ_P(zv))->state == PHASYNC_S_READY) {
			Z_ADDREF_P(zv);
			if (key) {
				zend_hash_update(Z_ARRVAL_P(return_value), key, zv);
			} else {
				zend_hash_index_update(Z_ARRVAL_P(return_value), h, zv);
			}
		}
	} ZEND_HASH_FOREACH_END();
}

/* ---- the other methods -------------------------------------------------------------- */

ZEND_METHOD(phasync_ext_Suspension, __construct)
{
	ZEND_PARSE_PARAMETERS_NONE();
}

static bool phasync_memcached_sessions_unsafe(void);
static void phasync_wrap_existing_streams(bool include_files);
static void phasync_install_hooks(void);

ZEND_METHOD(phasync_ext_Suspension, manage)
{
	zval *zfiber;
	zend_fiber *f;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(zfiber, zend_ce_fiber)
	ZEND_PARSE_PARAMETERS_END();

	if (!PHASYNC_G(managing)) {
		if (phasync_memcached_sessions_unsafe()) {
			zend_throw_exception(spl_ce_LogicException, "memcached.sess_persistent=1 with "
				"session.save_handler=memcached shares a persistent connection between "
				"fibers without the per-handle lock, and its session lock wait blocks "
				"the process; Suspension::manage() refuses to run with this combination -- set "
				"memcached.sess_persistent=0, or use another session handler (see README, "
				"Memcached)", 0);
			RETURN_THROWS();
		}
		PHASYNC_G(managing) = true;
		phasync_install_hooks();
		/* Streams that appeared after RINIT's walk: the STDIN/STDOUT/STDERR constants,
		 * which the CLI creates once execution starts, and files opened since. */
		phasync_wrap_existing_streams(true);
	}
	f = (zend_fiber *) Z_OBJ_P(zfiber);
	f->flags |= PHASYNC_FIBER_MANAGED;
	RETURN_COPY(zfiber);
}

static phasync_susp *phasync_this_susp(zval *this_zv)
{
	return phasync_susp_from(Z_OBJ_P(this_zv));
}

ZEND_METHOD(phasync_ext_Suspension, isReady)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(phasync_this_susp(ZEND_THIS)->state == PHASYNC_S_READY);
}

/* Resume (or throw into) the fiber it suspended, as Fiber::resume()/throw() would. */
static void phasync_susp_continue(INTERNAL_FUNCTION_PARAMETERS, zval *exception)
{
	phasync_susp *s = phasync_this_susp(ZEND_THIS);
	zval fiber;

	if (s->state == PHASYNC_S_RETIRED || s->fiber == NULL) {
		zend_throw_exception(spl_ce_LogicException, "This Suspension was already resumed", 0);
		RETURN_THROWS();
	}
	if (exception == NULL && s->state != PHASYNC_S_READY) {
		zend_throw_exception(spl_ce_LogicException, "This Suspension is not ready", 0);
		RETURN_THROWS();
	}
	ZVAL_OBJ_COPY(&fiber, &s->fiber->std);
	if (exception) {
		zend_call_method_with_1_params(Z_OBJ(fiber), zend_ce_fiber, NULL, "throw", return_value, exception);
	} else {
		zend_call_method_with_0_params(Z_OBJ(fiber), zend_ce_fiber, NULL, "resume", return_value);
	}
	zval_ptr_dtor(&fiber);
}

ZEND_METHOD(phasync_ext_Suspension, resume)
{
	ZEND_PARSE_PARAMETERS_NONE();
	phasync_susp_continue(INTERNAL_FUNCTION_PARAM_PASSTHRU, NULL);
}

ZEND_METHOD(phasync_ext_Suspension, throw)
{
	zval *exception;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(exception, zend_ce_throwable)
	ZEND_PARSE_PARAMETERS_END();
	phasync_susp_continue(INTERNAL_FUNCTION_PARAM_PASSTHRU, exception);
}

static void phasync_susp_wait_method(INTERNAL_FUNCTION_PARAMETERS, int dir)
{
	zval *zstream;
	double timeout = INFINITY;
	bool timeout_null = true;
	php_stream *stream;
	php_socket_t fd;
	phasync_poller *p;
	int rc;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_RESOURCE(zstream)
		Z_PARAM_OPTIONAL
		Z_PARAM_DOUBLE_OR_NULL(timeout, timeout_null)
	ZEND_PARSE_PARAMETERS_END();

	php_stream_from_zval(stream, zstream);
	if (timeout_null || timeout >= DBL_MAX) {
		timeout = INFINITY;
	} else if (timeout < 0) {
		timeout = 0;
	}
	/* Data already buffered in the stream is readable without waiting for the descriptor. */
	if (dir == PHASYNC_READ && stream->writepos > stream->readpos) {
		RETURN_TRUE;
	}
	if ((fd = phasync_stream_fd(stream)) == -1) {
		zend_value_error("The stream has no file descriptor to wait on");
		RETURN_THROWS();
	}
	if (phasync_managed_top() == NULL) {
		RETURN_BOOL(phasync_wait_inline(fd, dir, timeout) != PHASYNC_WAIT_TIMEOUT);
	}
	if ((p = phasync_proc_poller()) == NULL) {
		zend_throw_error(NULL, "Unable to create the epoll set: %s", strerror(errno));
		RETURN_THROWS();
	}
	rc = phasync_poller_wait(p, dir, stream, fd, timeout, NULL, true);
	if (rc == PHASYNC_WAIT_ERROR) {
		RETURN_THROWS();
	}
	RETURN_BOOL(rc != PHASYNC_WAIT_TIMEOUT);
}

ZEND_METHOD(phasync_ext_Suspension, readable)
{
	phasync_susp_wait_method(INTERNAL_FUNCTION_PARAM_PASSTHRU, PHASYNC_READ);
}

ZEND_METHOD(phasync_ext_Suspension, writable)
{
	phasync_susp_wait_method(INTERNAL_FUNCTION_PARAM_PASSTHRU, PHASYNC_WRITE);
}

/* ---- lifecycle -------------------------------------------------------------------------- */

static void phasync_suspension_minit(void)
{
	phasync_suspension_ce = register_class_phasync_ext_Suspension(zend_ce_exception);
	phasync_suspension_ce->create_object = phasync_suspension_create;
	memcpy(&phasync_suspension_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	phasync_suspension_handlers.offset = offsetof(phasync_susp, std);
	phasync_suspension_handlers.free_obj = phasync_suspension_free;
	phasync_suspension_handlers.dtor_obj = phasync_suspension_dtor;
	phasync_suspension_handlers.clone_obj = NULL;
}

static void phasync_suspension_rinit(void)
{
	PHASYNC_G(susp) = NULL;
	PHASYNC_G(susp_spare) = NULL;
	PHASYNC_G(nsusp) = PHASYNC_G(capsusp) = PHASYNC_G(nspare) = 0;
	PHASYNC_G(orphans) = NULL;
	PHASYNC_G(norphans) = PHASYNC_G(caporphans) = 0;
	PHASYNC_G(proc_poller) = NULL;
	PHASYNC_G(managing) = false;
	memset(&PHASYNC_G(proc_scope), 0, sizeof(PHASYNC_G(proc_scope)));
	ZVAL_UNDEF(&PHASYNC_G(proc_scope).sleep);
}

static void phasync_suspension_rshutdown(void)
{
	uint32_t n = PHASYNC_G(nsusp);

	/* Fibers released here may be destroyed, unwinding through phasync_park(): retire
	 * everything first so that touches nothing. */
	for (uint32_t i = 0; i < n; i++) {
		PHASYNC_G(susp)[i]->state = PHASYNC_S_RETIRED;
	}
	phasync_orphans_release();
	for (uint32_t i = 0; i < n; i++) {
		phasync_susp *s = PHASYNC_G(susp)[i];
		if (s->fiber) {
			zend_fiber *f = s->fiber;
			s->fiber = NULL;
			OBJ_RELEASE(&f->std);
		}
	}
	phasync_orphans_release();
	for (uint32_t i = 0; i < n; i++) {
		phasync_susp *s = PHASYNC_G(susp)[i];
		if (s->owned) {
			OBJ_RELEASE(&s->std);
		}
		/* else the loop still holds it: PHP frees it with the rest of the request */
	}
	if (PHASYNC_G(susp)) {
		efree(PHASYNC_G(susp));
		efree(PHASYNC_G(susp_spare));
	}
	if (PHASYNC_G(orphans)) {
		efree(PHASYNC_G(orphans));
	}
	PHASYNC_G(orphans) = NULL;
	PHASYNC_G(norphans) = PHASYNC_G(caporphans) = 0;
	PHASYNC_G(susp) = NULL;
	PHASYNC_G(susp_spare) = NULL;
	PHASYNC_G(nsusp) = PHASYNC_G(capsusp) = PHASYNC_G(nspare) = 0;
	if (PHASYNC_G(proc_poller)) {
		phasync_poller *p = PHASYNC_G(proc_poller);
		PHASYNC_G(proc_poller) = NULL;
		OBJ_RELEASE(&p->std);
	}
	PHASYNC_G(managing) = false;
}
