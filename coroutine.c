/* Included by phasync.c (single translation unit); not compiled on its own. */

/* Preemption (set_preempt_function()/checkpoint(), the timer thread, the compile-time checkpoint injection), and the fiber observer settling ledgers. */

/* ---- fiber observers ---- */

/* Settle what a ledger still holds: its coroutine is gone without its code after
 * the suspension having run (see the ledger). Its slots are taken back first. */
static void phasync_ledger_settle(phasync_ledger *l)
{
	if (l->task) {
		phasync_task_finish(l->task, l->task_p, NULL);
		l->task = NULL;
	}
	if (l->wait_p) {
		phasync_chan_purge(l->wait_p->chan, l->wait_slot);
		phasync_wait_settle(l->wait_p, l->wait_idx, l->wait_stream, l->wait_fd, l->wait_slot, l->wait_dupfd);
		l->wait_p = NULL;
	}
	if (l->inflight) {
		phasync_inflight_out(l->inflight);
		l->inflight = NULL;
	}
	if (l->closing) {
		phasync_inflight *f = zend_hash_index_find_ptr(&PHASYNC_G(inflight), (zend_ulong) (uintptr_t) l->closing);
		if (f && f->closer == l->closing_p) {
			phasync_chan_purge(f->closer->chan, f->closer_slot);
			f->closer = NULL;
			if (f->hold) {
				phasync_hold_drop(f);
			}
		}
		OBJ_RELEASE(&l->closing_p->std);
		l->closing = NULL;
	}
}

static void phasync_ledger_close(zend_fiber_context *ctx)
{
	phasync_ledger *l = zend_hash_index_find_ptr(&PHASYNC_G(ledgers), (zend_ulong) (uintptr_t) ctx);

	if (l) {
		phasync_ledger_settle(l);
		zend_hash_index_del(&PHASYNC_G(ledgers), (zend_ulong) (uintptr_t) ctx);
	}
}

static void phasync_ledger_fiber_destroy(zend_fiber_context *ctx)
{
	phasync_ledger_close(ctx);
}

/* set_preempt_function(): every backward jump (a loop's back-edge, a backward
 * goto, foreach's continue) is compiled with
 *     if (\phasync\ext\__PREEMPT_DUE) \phasync\ext\checkpoint();
 * right before it (phasync_inject_checkpoints()). The constant is registered
 * per request, without CONST_PERSISTENT, so neither the compiler nor opcache
 * substitutes it; FETCH_CONSTANT reads it through its run-time cache slot, and
 * both JITs compile that into loads and a compare of its type byte, without a
 * call. A timer thread raises it at most every interval by storing IS_TRUE into
 * its type_info (an aligned 32-bit store, single-copy atomic on x86-64 and
 * aarch64; no data is published through it, so relaxed ordering is enough: the
 * checkpoint re-checks `pending` and takes the mutex). The next checkpoint that
 * runs calls the closure if the stack allows it. The interval counts from the
 * moment the closure is due there: the thread raises one request at a time.
 *
 * A checkpoint the stack rules reject lowers the flag (so the checkpoints that
 * follow cost a load again) and the thread raises it again after
 * PHASYNC_PREEMPT_RETRY. */
#define PHASYNC_PREEMPT_RETRY_NS 250000L    /* 0.25 ms */

static zend_always_inline void phasync_flag_store(uint32_t *flag, uint32_t type)
{
	__atomic_store_n(flag, type, __ATOMIC_RELAXED);
}

static zend_always_inline bool phasync_flag_raised(void)
{
	return __atomic_load_n(&Z_TYPE_INFO_P(PHASYNC_G(preempt_due)), __ATOMIC_RELAXED) == IS_TRUE;
}

typedef struct phasync_preempt {
	pthread_mutex_t   mutex;
	pthread_cond_t    cond;
	pthread_t         thread;
	uint32_t         *flag;           /* type_info of the owning PHP thread's __PREEMPT_DUE */
	zend_atomic_bool  pending;        /* raised, not yet taken at a checkpoint    */
	bool              stop;
	double            interval;
	struct timespec   since;          /* start of the current interval (CLOCK_MONOTONIC) */
} phasync_preempt;

static zend_string *phasync_uninterruptible_lcname;   /* the attribute, as attributes are keyed */

static zend_always_inline void phasync_ts_add_ns(struct timespec *ts, long ns)
{
	ts->tv_nsec += ns;
	while (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* pthread_cond_timedwait() until a CLOCK_MONOTONIC deadline (macOS has no
 * pthread_condattr_setclock(): a relative wait there). */
static int phasync_cond_wait_until(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline)
{
#ifdef __APPLE__
	struct timespec now, rel;

	clock_gettime(CLOCK_MONOTONIC, &now);
	rel.tv_sec = deadline->tv_sec - now.tv_sec;
	rel.tv_nsec = deadline->tv_nsec - now.tv_nsec;
	if (rel.tv_nsec < 0) {
		rel.tv_sec--;
		rel.tv_nsec += 1000000000L;
	}
	if (rel.tv_sec < 0) {
		return ETIMEDOUT;
	}
	return pthread_cond_timedwait_relative_np(cond, mutex, &rel);
#else
	return pthread_cond_timedwait(cond, mutex, deadline);
#endif
}

static void *phasync_preempt_main(void *arg)
{
	phasync_preempt *p = arg;
	struct timespec now, deadline;

#ifdef __APPLE__
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   /* no timer coalescing of its waits */
#endif
	pthread_mutex_lock(&p->mutex);
	while (!p->stop) {
		if (zend_atomic_bool_load_ex(&p->pending)) {
			/* Not taken yet: no checkpoint ran, or the stack rules rejected
			 * it; raise the flag again shortly. */
			clock_gettime(CLOCK_MONOTONIC, &deadline);
			phasync_ts_add_ns(&deadline, PHASYNC_PREEMPT_RETRY_NS);
			if (phasync_cond_wait_until(&p->cond, &p->mutex, &deadline) == ETIMEDOUT
			 && zend_atomic_bool_load_ex(&p->pending)) {
				phasync_flag_store(p->flag, IS_TRUE);
			}
			continue;
		}
		deadline = p->since;
		deadline.tv_sec += (time_t) p->interval;
		phasync_ts_add_ns(&deadline, (long) ((p->interval - floor(p->interval)) * 1e9));
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
			zend_atomic_bool_store_ex(&p->pending, true);
			phasync_flag_store(p->flag, IS_TRUE);
			continue;
		}
		phasync_cond_wait_until(&p->cond, &p->mutex, &deadline);
	}
	pthread_mutex_unlock(&p->mutex);
	return NULL;
}

/* Start a timer for this PHP thread; NULL if no thread could be created. The
 * thread blocks every signal, so process signals (pcntl, SIGPROF of
 * max_execution_time) keep going to PHP's thread. */
static phasync_preempt *phasync_preempt_start(double interval)
{
	phasync_preempt *p = calloc(1, sizeof(*p));
	pthread_condattr_t ca;
	sigset_t all, old;
	int rc;

	if (!p) {
		return NULL;
	}
	pthread_mutex_init(&p->mutex, NULL);
	pthread_condattr_init(&ca);
#ifndef __APPLE__
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
	pthread_cond_init(&p->cond, &ca);
	pthread_condattr_destroy(&ca);
	p->flag = &Z_TYPE_INFO_P(PHASYNC_G(preempt_due));
	zend_atomic_bool_init(&p->pending, false);
	p->interval = interval;
	clock_gettime(CLOCK_MONOTONIC, &p->since);
	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &old);
	rc = pthread_create(&p->thread, NULL, phasync_preempt_main, p);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	if (rc != 0) {
		pthread_cond_destroy(&p->cond);
		pthread_mutex_destroy(&p->mutex);
		free(p);
		return NULL;
	}
	return p;
}

static void phasync_preempt_stop(phasync_preempt *p)
{
	pthread_mutex_lock(&p->mutex);
	p->stop = true;
	pthread_cond_signal(&p->cond);
	pthread_mutex_unlock(&p->mutex);
	pthread_join(p->thread, NULL);
	pthread_cond_destroy(&p->cond);
	pthread_mutex_destroy(&p->mutex);
	free(p);
	phasync_flag_store(&Z_TYPE_INFO_P(PHASYNC_G(preempt_due)), IS_FALSE);
}

/* fork(): the child has no timer thread. It gets one of its own, with the same
 * interval; the parent's mutex is held across fork() so the child's copy is in
 * a known state (owned by the forking thread, which the child has). */
static void phasync_preempt_fork_prepare(void)
{
	if (PHASYNC_G(preempt)) {
		pthread_mutex_lock(&PHASYNC_G(preempt)->mutex);
	}
}

static void phasync_preempt_fork_parent(void)
{
	if (PHASYNC_G(preempt)) {
		pthread_mutex_unlock(&PHASYNC_G(preempt)->mutex);
	}
}

static void phasync_preempt_fork_child(void)
{
	phasync_preempt *old = PHASYNC_G(preempt);

	if (old) {
		/* old's cond may record the parent's timer thread as a waiter: it is
		 * dropped as plain memory, not destroyed. If no thread can be created,
		 * the child runs without preemption. */
		PHASYNC_G(preempt) = phasync_preempt_start(old->interval);
		free(old);
	}
}

/* #[phasync\Uninterruptible], or a destructor. A due checkpoint is rare (about
 * once per 1 ms), so this attribute/name lookup runs fresh each time instead
 * of being cached: op_array run-time caches are per closure object (a rebound
 * closure gets its own, zeroed one), so caching this in the run-time cache
 * via the arena, as before, allocated on CG(arena) -- never freed for the
 * life of a long-running worker -- once per closure that hit a checkpoint. */
static bool phasync_op_array_no_preempt(const zend_op_array *op_array)
{
	return (op_array->attributes
		&& zend_get_attribute(op_array->attributes, phasync_uninterruptible_lcname))
		|| (op_array->scope && op_array->function_name
		 && zend_string_equals_literal_ci(op_array->function_name, "__destruct"));
}

/* Preemption happens only at checkpoints (between loop iterations), where
 * phasync_never_suspend() allows it: the same never-list as for a wait, plus no
 * destructor, no function with #[phasync\Uninterruptible], and outside fibers none
 * at the bottom of the stack (an exception handler, a shutdown function). */
static bool phasync_preemptible(const zend_execute_data *execute_data)
{
	return !phasync_never_suspend(execute_data, true);
}

/* A raised checkpoint: ex is the PHP frame it is in. With own_frame, the
 * checkpoint was called with a frame of its own (EG(current_execute_data)),
 * which is left out of the stack while the closure runs; without, the frame's
 * opline is the frameless call, moved past it while the closure runs so no
 * backtrace shows it. */
static zend_never_inline void phasync_checkpoint_hit(zend_execute_data *ex, bool own_frame)
{
	phasync_preempt *p = PHASYNC_G(preempt);
	zend_execute_data *saved_ex;
	const zend_op *saved_op;
	zval fn, retval;

	phasync_flag_store(&Z_TYPE_INFO_P(PHASYNC_G(preempt_due)), IS_FALSE);
	if (!p || !zend_atomic_bool_load_ex(&p->pending)) {
		return;
	}
	/* Uninterruptible here, or an exception is on its way: the thread asks
	 * again after PHASYNC_PREEMPT_RETRY. */
	if (EG(exception) || !ex || !phasync_preemptible(ex)) {
		return;
	}
	pthread_mutex_lock(&p->mutex);
	zend_atomic_bool_store_ex(&p->pending, false);
	clock_gettime(CLOCK_MONOTONIC, &p->since);
	pthread_cond_signal(&p->cond);
	pthread_mutex_unlock(&p->mutex);

	/* Where Fiber::suspend() would throw (pcntl handlers, a fiber being
	 * destroyed), skip: the next interval gets it. */
	if (zend_fiber_switch_blocked() || phasync_unwinding()) {
		return;
	}
	saved_ex = EG(current_execute_data);
	saved_op = ex->opline;
	if (own_frame) {
		EG(current_execute_data) = ex;
	} else {
		ex->opline = saved_op + 1;
	}
	ZVAL_COPY(&fn, &PHASYNC_G(preempt_fn));   /* it may replace itself */
	zend_hash_index_add_empty_element(&phasync_suspend_roots, (zend_ulong) (uintptr_t) EG(current_execute_data));
	call_user_function(NULL, NULL, &fn, &retval, 0, NULL);
	zend_hash_index_del(&phasync_suspend_roots, (zend_ulong) (uintptr_t) EG(current_execute_data));
	zval_ptr_dtor(&retval);
	zval_ptr_dtor(&fn);
	if (own_frame) {
		EG(current_execute_data) = saved_ex;
	} else if (!EG(exception)) {
		ex->opline = saved_op;
	} else if (EG(opline_before_exception) == saved_op + 1) {
		EG(opline_before_exception) = saved_op;
	}
}

/* The checkpoint, called when __PREEMPT_DUE was seen raised. PHP 8.4+ calls it
 * frameless and has it in no function table; older PHP calls it with a frame and
 * by name, so PHP code can call it too (hence the check). */
ZEND_FUNCTION(phasync_checkpoint)
{
	if (EXPECTED(!phasync_flag_raised())) {
		return;
	}
	phasync_checkpoint_hit(execute_data->prev_execute_data, true);
}

#if PHP_VERSION_ID >= 80400
ZEND_FRAMELESS_FUNCTION(phasync_checkpoint, 0)
{
	if (EXPECTED(!phasync_flag_raised())) {
		return;
	}
	phasync_checkpoint_hit(EG(current_execute_data), false);
}

static const zend_frameless_function_info phasync_checkpoint_flf[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(phasync_checkpoint, 0), 0 },
	{ 0 },
};
static uint32_t phasync_checkpoint_flf_offset;
#endif
static zend_function *phasync_checkpoint_func;

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_phasync_checkpoint, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry phasync_checkpoint_functions[] = {
	{ .fname = "phasync\\ext\\checkpoint", .handler = ZEND_FN(phasync_checkpoint),
	  .arg_info = arginfo_phasync_checkpoint, .num_args = 0, .flags = 0,
#if PHP_VERSION_ID >= 80400
	  .frameless_function_infos = phasync_checkpoint_flf,
#endif
	},
	ZEND_FE_END
};
static zend_string *phasync_checkpoint_name;
static zend_string *phasync_preempt_due_name;
#if PHP_VERSION_ID >= 80400
static zend_result (*phasync_prev_post_startup_cb)(void);

/* Chained onto zend_post_startup_cb (see phasync_checkpoint_startup()): hides
 * checkpoint() from CG(function_table) once it is safe to. */
static zend_result phasync_checkpoint_hide(void)
{
	dtor_func_t dtor = CG(function_table)->pDestructor;

	CG(function_table)->pDestructor = NULL;
	zend_hash_del(CG(function_table), phasync_checkpoint_name);
	CG(function_table)->pDestructor = dtor;
	/* Close the hole it leaves (a module registered after this one has its
	 * functions behind it). The engine counts the persistent functions right
	 * after this callback and removes the user functions past that count at the
	 * end of each request; a hole would be squeezed out by the first compaction
	 * of the table in a request, moving the first user function below the
	 * count, where it stays after the request (#35). */
	zend_hash_rehash(CG(function_table));
	return phasync_prev_post_startup_cb ? phasync_prev_post_startup_cb() : SUCCESS;
}
#endif

/* Compile time, before opcache optimizes and caches the op_array: pass_two()
 * calls this first, while jump targets are still opline numbers and break,
 * continue and goto still unresolved. */

/* Where a raw jump op goes, or -1 if it can't jump backward. */
static uint32_t phasync_raw_backward_target(const zend_op_array *op_array, const zend_op *op)
{
	switch (op->opcode) {
		case ZEND_JMP:
			return op->op1.opline_num;
		case ZEND_JMPZ:
		case ZEND_JMPNZ:
		case ZEND_JMPZ_EX:
		case ZEND_JMPNZ_EX:
			return op->op2.opline_num;
		case ZEND_GOTO: {
			zval *label = CT_CONSTANT_EX(op_array, op->op2.constant);
			zend_label *dest = CG(context).labels ? zend_hash_find_ptr(CG(context).labels, Z_STR_P(label)) : NULL;

			return dest ? dest->opline_num : (uint32_t) -1;
		}
		case ZEND_CONT: {
			int nest_levels = op->op2.num, array_offset = op->op1.num;
			zend_brk_cont_element *jmp_to;

			do {
				jmp_to = &CG(context).brk_cont_array[array_offset];
				if (nest_levels > 1) {
					array_offset = jmp_to->parent;
				}
			} while (--nest_levels > 0);
			return jmp_to->cont;
		}
		default:
			return (uint32_t) -1;
	}
}

/* Apply map to every opline number a raw op_array refers to. */
static void phasync_raw_map_targets(zend_op_array *op_array, uint32_t (*map)(uint32_t, void *), void *arg)
{
	for (uint32_t i = 0; i < op_array->last; i++) {
		zend_op *op = &op_array->opcodes[i];

		switch (op->opcode) {
			case ZEND_JMP:
				op->op1.opline_num = map(op->op1.opline_num, arg);
				break;
			case ZEND_CATCH:
				if (op->extended_value & ZEND_LAST_CATCH) {
					break;
				}
				ZEND_FALLTHROUGH;
			case ZEND_JMPZ:
			case ZEND_JMPNZ:
			case ZEND_JMPZ_EX:
			case ZEND_JMPNZ_EX:
			case ZEND_JMP_SET:
			case ZEND_COALESCE:
			case ZEND_FE_RESET_R:
			case ZEND_FE_RESET_RW:
			case ZEND_JMP_NULL:
			case ZEND_ASSERT_CHECK:
			case ZEND_BIND_INIT_STATIC_OR_JMP:
#if PHP_VERSION_ID >= 80400
			case ZEND_JMP_FRAMELESS:
#endif
				op->op2.opline_num = map(op->op2.opline_num, arg);
				break;
			case ZEND_FE_FETCH_R:
			case ZEND_FE_FETCH_RW:
				op->extended_value = map(op->extended_value, arg);
				break;
			case ZEND_SWITCH_LONG:
			case ZEND_SWITCH_STRING:
			case ZEND_MATCH: {
				zval *zv;

				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(CT_CONSTANT_EX(op_array, op->op2.constant)), zv) {
					Z_LVAL_P(zv) = map((uint32_t) Z_LVAL_P(zv), arg);
				} ZEND_HASH_FOREACH_END();
				op->extended_value = map(op->extended_value, arg);
				break;
			}
		}
	}
	for (int i = 0; i < op_array->last_try_catch; i++) {
		zend_try_catch_element *tc = &op_array->try_catch_array[i];

		tc->try_op = map(tc->try_op, arg);
		tc->catch_op = map(tc->catch_op, arg);
		tc->finally_op = map(tc->finally_op, arg);
		tc->finally_end = map(tc->finally_end, arg);
	}
	for (int i = 0; i < CG(context).last_brk_cont; i++) {
		zend_brk_cont_element *bc = &CG(context).brk_cont_array[i];

		if (bc->start >= 0) {
			bc->start = (int) map((uint32_t) bc->start, arg);
		}
		if (bc->cont >= 0) {
			bc->cont = (int) map((uint32_t) bc->cont, arg);
		}
		if (bc->brk >= 0) {
			bc->brk = (int) map((uint32_t) bc->brk, arg);
		}
	}
	if (CG(context).labels) {
		zend_label *label;

		ZEND_HASH_FOREACH_PTR(CG(context).labels, label) {
			label->opline_num = map(label->opline_num, arg);
		} ZEND_HASH_FOREACH_END();
	}
}

static uint32_t phasync_mark_target(uint32_t t, void *arg)
{
	uint32_t *targeted = arg;

	if (t != (uint32_t) -1) {
		targeted[t / 32] |= 1u << (t % 32);
	}
	return t;
}

typedef struct {
	const uint32_t *at;   /* sorted insertion points */
	uint32_t n, k;        /* how many, ops per checkpoint */
} phasync_shift;

/* A target moves past the checkpoints inserted before it; one at an insertion
 * point lands on that point's checkpoint. */
static uint32_t phasync_shift_target(uint32_t t, void *arg)
{
	const phasync_shift *s = arg;
	uint32_t lo = 0, hi = s->n;

	while (lo < hi) {   /* how many insertion points are < t */
		uint32_t mid = (lo + hi) / 2;

		if (s->at[mid] < t) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return t + lo * s->k;
}

/* FETCH_CONSTANT __PREEMPT_DUE, JMPZ past the call, then the call:
 * FRAMELESS_ICALL_0 + FREE on PHP 8.4+, INIT_FCALL + DO_ICALL before. */
#define PHASYNC_CHECKPOINT_OPS 4

/* Append a literal (a string interned for the process) to a raw op_array. */
static uint32_t phasync_add_literal(zend_op_array *op_array, zend_string *str)
{
	uint32_t lit = op_array->last_literal;

	if (lit >= CG(context).literals_size) {
		CG(context).literals_size += 16;
		op_array->literals = erealloc(op_array->literals, CG(context).literals_size * sizeof(zval));
	}
	ZVAL_INTERNED_STR(&op_array->literals[lit], str);
	Z_EXTRA(op_array->literals[lit]) = 0;
	op_array->last_literal++;
	return lit;
}

static void phasync_inject_checkpoints(zend_op_array *op_array)
{
	uint32_t *at, *targeted, n = 0, last = op_array->last, k = PHASYNC_CHECKPOINT_OPS;
	zend_op *opcodes;
	phasync_shift shift;

	for (uint32_t i = 0; i < last; i++) {
		uint32_t t = phasync_raw_backward_target(op_array, &op_array->opcodes[i]);

		if (t <= i) {
			n++;
		}
	}
	if (!n) {
		return;
	}
	at = emalloc(n * sizeof(uint32_t));
	targeted = ecalloc(last / 32 + 1, sizeof(uint32_t));
	phasync_raw_map_targets(op_array, phasync_mark_target, targeted);
	n = 0;
	for (uint32_t i = 0; i < last; i++) {
		const zend_op *op = &op_array->opcodes[i];
		uint32_t t = phasync_raw_backward_target(op_array, op), p = i;

		if (t > i) {
			continue;
		}
		if (op->opcode == ZEND_GOTO) {
			p = i - op->op1.num;   /* before the frees goto may unwind with */
		} else if ((op->opcode == ZEND_JMPZ || op->opcode == ZEND_JMPNZ)
		 && op->op1_type == IS_TMP_VAR && i > 0 && !((targeted[i / 32] >> (i % 32)) & 1)
		 && (op[-1].result_type & IS_TMP_VAR) && op[-1].result.var == op->op1.var) {
			p = i - 1;         /* keep the condition next to its jump (smart branch) */
		}
		at[n++] = p;
	}
	efree(targeted);

	shift = (phasync_shift) { at, n, k };
	phasync_raw_map_targets(op_array, phasync_shift_target, &shift);

	opcodes = emalloc(sizeof(zend_op) * (last + n * k));
	for (uint32_t i = 0, j = 0, o = 0; i < last; i++) {
		while (j < n && at[j] == i) {
			const zend_op *jump = &op_array->opcodes[i];
			zend_op *cp = &opcodes[o];

			memset(cp, 0, sizeof(zend_op) * k);
			for (uint32_t c = 0; c < k; c++) {
				SET_UNUSED(cp[c].op1);
				SET_UNUSED(cp[c].op2);
				SET_UNUSED(cp[c].result);
				cp[c].lineno = jump->lineno;
			}
			/* FETCH_CONSTANT expects two literals: the name as written and the
			 * one to look up (namespace lowercased), the same here. */
			cp[0].opcode = ZEND_FETCH_CONSTANT;
			cp[0].op1.num = 0;              /* a fully qualified name */
			cp[0].op2_type = IS_CONST;
			cp[0].op2.constant = phasync_add_literal(op_array, phasync_preempt_due_name);
			phasync_add_literal(op_array, phasync_preempt_due_name);
			cp[0].result_type = IS_TMP_VAR;
			cp[0].result.var = op_array->T;
			cp[0].extended_value = op_array->cache_size;
			op_array->cache_size += sizeof(void *);
			cp[1].opcode = ZEND_JMPZ;
			cp[1].op1_type = IS_TMP_VAR;
			cp[1].op1.var = op_array->T++;
			cp[1].op2.opline_num = o + k;   /* the jump itself */
#if PHP_VERSION_ID >= 80400
			cp[2].opcode = ZEND_FRAMELESS_ICALL_0;
			cp[2].extended_value = phasync_checkpoint_flf_offset;
			cp[2].result_type = IS_TMP_VAR;
			cp[2].result.var = op_array->T;
			cp[3].opcode = ZEND_FREE;
			cp[3].op1_type = IS_TMP_VAR;
			cp[3].op1.var = op_array->T++;
#else
			cp[2].opcode = ZEND_INIT_FCALL;
			cp[2].op1.num = zend_vm_calc_used_stack(0, phasync_checkpoint_func);
			cp[2].op2_type = IS_CONST;
			cp[2].op2.constant = phasync_add_literal(op_array, phasync_checkpoint_name);
			cp[2].result.num = op_array->cache_size;
			op_array->cache_size += sizeof(void *);
			cp[3].opcode = ZEND_DO_ICALL;
#endif
			o += k;
			j++;
		}
		opcodes[o++] = op_array->opcodes[i];
	}
	efree(op_array->opcodes);
	op_array->opcodes = opcodes;
	op_array->last = last + n * k;
	CG(context).opcodes_size = op_array->last;
	efree(at);
}

static void phasync_op_array_handler(zend_op_array *op_array)
{
	phasync_inject_checkpoints(op_array);
}

static zend_extension phasync_zend_extension = {
	.name = "phasync",
	.version = PHP_PHASYNC_VERSION,
	.author = "phasync",
	.op_array_handler = phasync_op_array_handler,
	.resource_number = -1,
};

/* Registered in MINIT: the checkpoint function and the compile hook. */
static void phasync_checkpoint_startup(void)
{
	zend_register_functions(NULL, phasync_checkpoint_functions, NULL, MODULE_PERSISTENT);
	phasync_checkpoint_name = zend_string_init_interned(ZEND_STRL("phasync\\ext\\checkpoint"), 1);
	phasync_preempt_due_name = zend_string_init_interned(ZEND_STRL("phasync\\ext\\__PREEMPT_DUE"), 1);
	phasync_checkpoint_func = zend_hash_find_ptr(CG(function_table), phasync_checkpoint_name);
#if PHP_VERSION_ID >= 80400
	for (uint32_t i = 0; zend_flf_handlers[i]; i++) {
		if (zend_flf_handlers[i] == (void *) ZEND_FRAMELESS_FUNCTION_NAME(phasync_checkpoint, 0)) {
			phasync_checkpoint_flf_offset = i;
		}
	}
	/* Frameless calls reach it through zend_flf_handlers/zend_flf_functions, not
	 * by name, so it can come out of the function table (without freeing it,
	 * MSHUTDOWN does) -- but not yet. zend_observer_post_startup() (main.c,
	 * called after every module's MINIT, including this one) walks every
	 * function still in CG(function_table) and gives it the extra call-frame
	 * temp (common.T) an observed function needs to link its caller's observed
	 * frame (zend_observer.c's prev_observed_frame()). Removed here already,
	 * checkpoint's T stays 0; a checkpoint() call that Xdebug's observer sees
	 * then has prev_observed_frame() index the frame at (uint32_t) -1 and write
	 * through it, corrupting memory near the call. zend_post_startup_cb runs
	 * right after zend_observer_post_startup(), while the function table is
	 * still the writable one MINIT used (zend_post_startup() only freezes a
	 * read-only copy for ZTS afterward): chain onto it to hide checkpoint()
	 * once its T is set. */
	phasync_prev_post_startup_cb = zend_post_startup_cb;
	zend_post_startup_cb = phasync_checkpoint_hide;
#endif
	zend_register_extension(&phasync_zend_extension, NULL);
}

ZEND_FUNCTION(phasync_ext_set_preempt_function)
{
	zend_object *fn;
	double interval = 0.1;
	phasync_preempt *p;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_OBJ_OF_CLASS_OR_NULL(fn, zend_ce_closure)
		Z_PARAM_OPTIONAL
		Z_PARAM_DOUBLE(interval)
	ZEND_PARSE_PARAMETERS_END();

	if (!(interval > 0) || !zend_finite(interval)) {
		zend_argument_value_error(2, "must be a positive number of seconds");
		RETURN_THROWS();
	}
	p = PHASYNC_G(preempt);
	if (fn && !p) {
		if (!(p = phasync_preempt_start(interval))) {
			zend_throw_error(NULL, "Could not start the preempt timer thread");
			RETURN_THROWS();
		}
		PHASYNC_G(preempt) = p;
	} else if (fn) {
		pthread_mutex_lock(&p->mutex);
		p->interval = interval;
		pthread_cond_signal(&p->cond);
		pthread_mutex_unlock(&p->mutex);
	} else if (p) {
		PHASYNC_G(preempt) = NULL;
		phasync_preempt_stop(p);
	}
	if (Z_TYPE(PHASYNC_G(preempt_fn)) == IS_UNDEF) {
		RETVAL_NULL();
	} else {
		ZVAL_COPY_VALUE(return_value, &PHASYNC_G(preempt_fn));
	}
	if (fn) {
		ZVAL_OBJ_COPY(&PHASYNC_G(preempt_fn), fn);
	} else {
		ZVAL_UNDEF(&PHASYNC_G(preempt_fn));
	}
}


