/* Included by phasync.c (single translation unit); not compiled on its own. */

/* The thread pool that runs blocking syscalls off PHP's own thread, and the task lifecycle (phasync_task_*) submitted to it. */

/* ---- thread pool: run blocking syscalls off the main thread --------------
 *
 * Workers only ever touch raw fds + plain buffers + the task struct — never the
 * Zend engine — so this is safe under a non-ZTS build. Completion is delivered
 * via a per-op self-pipe: the worker writes one byte to the pipe, which makes
 * the read end readable; the fiber was parked on that fd via the registered read
 * handler, so the scheduler resumes it. No lost-wakeup race: the byte is latched
 * in the pipe whether the worker finishes before or after the fiber parks.
 *
 * Fork safety: pthread_atfork() resets the pool in the child, because fork()
 * does not clone the worker threads and may copy the mutex in a locked state.
 * fork()+exec() (shell_exec/exec/proc_open) is unaffected — exec wipes the child
 * before it can touch the pool; only pcntl_fork() without exec needs the reset. */

/* Bounded worker pool for bounded-duration blocking syscalls (file read/write,
 * DNS). Size is INI-tunable (phasync.thread_pool_size); the cost of an idle
 * worker is a virtual stack reservation, so a larger pool is cheap — we also
 * cap each worker stack low (workers only call read/write/open/getaddrinfo).
 * FIFO open() is NOT run here: its rendezvous can block indefinitely and a
 * bounded pool would deadlock (all workers parked on read-opens while the
 * matching write-opens sit in the queue), so those get a dedicated thread. */
#define PHASYNC_POOL_MAX        128
#define PHASYNC_WORKER_STACK    (256 * 1024)



static pthread_mutex_t phasync_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  phasync_pool_cond  = PTHREAD_COND_INITIALIZER;
static phasync_task   *phasync_q_head, *phasync_q_tail;
static pthread_t       phasync_workers[PHASYNC_POOL_MAX];
static int             phasync_pool_n;        /* live worker count             */
static int             phasync_pool_started; /* 0 = not yet, 1 = running */
static int             phasync_pool_stop;

/* Run one task's blocking syscall (shared by pool workers, the FIFO-open
 * dedicated thread, and the inline fallback). Touches only fds/buffers. */
/* ---- a thread's task: owned by the extension, not by the coroutine ----------
 *
 * A task handed to a thread is a heap copy with its own buffer, its own copy of
 * the host name and its own duplicate of the descriptor: the thread never points
 * into the coroutine's frame or into PHP's data, and the descriptor can't be
 * closed (and its number reused) under it. The waiter collects the results once
 * the thread is done. If the waiter stops waiting first (cancelled, destroyed, or
 * its fiber gone without coming back), the task is orphaned: the thread frees it
 * when it finishes, and nobody waits for that. */
static phasync_task *phasync_task_own(phasync_task *t)
{
	phasync_task *h = malloc(sizeof(*h));

	if (h == NULL) {
		return NULL;
	}
	*h = *t;
	h->orphaned = 0;
	h->dedicated = false;
	h->ownfd = -1;
	h->host = NULL;
	h->buf = NULL;
	h->ai = NULL;
	if (t->type == PHASYNC_OP_OPEN) {
		h->fd = -1;
	}
	bool host = t->type == PHASYNC_OP_GETHOSTBYNAME || t->type == PHASYNC_OP_GETADDRINFO
	         || t->type == PHASYNC_OP_DNSQUERY;
	bool buf = t->type == PHASYNC_OP_READ || t->type == PHASYNC_OP_WRITE || t->type == PHASYNC_OP_DNSQUERY;
	if ((host && (h->host = strdup(t->host)) == NULL)
	 || (buf && (h->buf = malloc(t->count ? t->count : 1)) == NULL)) {
		goto fail;
	}
	if (t->type == PHASYNC_OP_WRITE) {
		memcpy(h->buf, t->buf, t->count);
	}
	if (t->type == PHASYNC_OP_READ || t->type == PHASYNC_OP_WRITE
	 || (t->type == PHASYNC_OP_FS && (t->fsop == PHASYNC_FS_FSYNC || t->fsop == PHASYNC_FS_FDATASYNC))) {
		if ((h->ownfd = h->fd = fcntl(t->fd, F_DUPFD_CLOEXEC, 0)) < 0) {
			goto fail;
		}
	}
	return h;
fail:
	free(h->buf);
	free((char *) h->host);
	free(h);
	return NULL;
}

/* From any thread: free a task and whatever it still owns. */
static void phasync_task_free(phasync_task *h)
{
	free(h->buf);
	free((char *) h->host);
	if (h->ownfd >= 0) {
		close(h->ownfd);
	}
	if (h->type == PHASYNC_OP_OPEN && h->fd >= 0) {
		close(h->fd);
	}
	if (h->ai) {
		freeaddrinfo(h->ai);
	}
	free(h);
}

/* A task is done: mark it done and queue its slot for poll(); an orphan the thread
 * frees instead. The thread never touches the task after this: its waiter may
 * free it at once. */
static void phasync_task_signal(phasync_task *t)
{
	zend_long slot = t->slot;
	phasync_chan *ch = t->chan;
	int orphaned;

	pthread_mutex_lock(&ch->mutex);
	t->done = 1;
	orphaned = t->orphaned;
	pthread_mutex_unlock(&ch->mutex);
	if (orphaned == 1) {
		phasync_task_free(t);
	} else if (orphaned == 0) {
		phasync_chan_push(ch, slot);
	}                                    /* 2: its waiter blocks for it */
	phasync_chan_release(ch);            /* the task's reference */
}

/* The waiter is done with its task (and lets go of the Poller): with the thread
 * done, copy the results into out (NULL: drop them) and free it; otherwise orphan
 * it (a dedicated thread is cancelled: it may block forever) and fail out with
 * ECANCELED. Never waits, but for a CALL. */
static bool phasync_task_finish(phasync_task *h, phasync_poller *p, phasync_task *out)
{
	phasync_chan *ch = h->chan;
	zend_long slot = h->slot;
	pthread_t th = h->thread;
	bool dedicated = h->dedicated;
	int done;

	pthread_mutex_lock(&ch->mutex);
	/* A call works on its caller's data: it is waited for, blocking, to its end. */
	while (!h->done && h->type == PHASYNC_OP_CALL) {
		h->orphaned = 2;
		pthread_mutex_unlock(&ch->mutex);
		usleep(50);
		pthread_mutex_lock(&ch->mutex);
	}
	done = h->done;
	if (!done) {
		h->orphaned = 1;                 /* the thread frees it from here on */
	}
	pthread_mutex_unlock(&ch->mutex);
	if (dedicated) {
		if (!done) {
			pthread_cancel(th);
		}
		pthread_detach(th);
	}
	phasync_chan_purge(p->chan, slot);
	OBJ_RELEASE(&p->std);
	if (!done) {
		if (out) {
			out->result = -1;
			out->err = ECANCELED;
			out->hostok = 0;
			if (out->type == PHASYNC_OP_OPEN) {
				out->fd = -1;
			}
		}
		return false;
	}
	if (out) {
		if ((h->type == PHASYNC_OP_READ || h->type == PHASYNC_OP_DNSQUERY) && h->result > 0) {
			memcpy(out->buf, h->buf, MIN((size_t) h->result, out->count));
		}
		out->result = h->result;
		out->err = h->err;
		out->hostok = h->hostok;
		out->naddrs = h->naddrs;
		memcpy(out->addrs, h->addrs, sizeof(h->addrs));
		memcpy(out->hostresult, h->hostresult, sizeof(h->hostresult));
		out->ai = h->ai;
		h->ai = NULL;
		if (h->type == PHASYNC_OP_OPEN) {
			out->fd = h->fd;
			h->fd = -1;
		}
	}
	phasync_task_free(h);
	return true;
}

/* Give a task its Poller's channel (a reference, released by the thread). */
static phasync_chan *phasync_task_chan(phasync_task *t, phasync_poller *p)
{
	phasync_chan *ch = p->chan;

	pthread_mutex_lock(&ch->mutex);
	ch->refs++;
	pthread_mutex_unlock(&ch->mutex);
	t->chan = ch;
	t->done = 0;
	return ch;
}

/* Block all signals on a helper thread, leaving them to the main thread. */
static void phasync_thread_block_signals(void)
{
	sigset_t set;
	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
}

static void *phasync_worker(void *arg)
{
	(void) arg;
	phasync_thread_block_signals();

	for (;;) {
		phasync_task *t;
		pthread_mutex_lock(&phasync_pool_mutex);
		while (phasync_q_head == NULL && !phasync_pool_stop) {
			pthread_cond_wait(&phasync_pool_cond, &phasync_pool_mutex);
		}
		if (phasync_pool_stop && phasync_q_head == NULL) {
			pthread_mutex_unlock(&phasync_pool_mutex);
			return NULL;
		}
		t = phasync_q_head;
		phasync_q_head = t->next;
		if (phasync_q_head == NULL) {
			phasync_q_tail = NULL;
		}
		pthread_mutex_unlock(&phasync_pool_mutex);

		phasync_task_exec(t);
		phasync_task_signal(t);   /* worker never touches the Zend engine */
	}
}

/* Cancelled inside open(): the task was orphaned (that is when it is cancelled). */
static void phasync_oneshot_cancelled(void *arg)
{
	phasync_task *t = (phasync_task *) arg;
	phasync_chan *ch = t->chan;

	phasync_task_free(t);                /* closes the descriptor if open() had returned */
	phasync_chan_release(ch);
}

/* Dedicated one-shot thread for a single indefinitely-blocking task (FIFO
 * open). It runs the task, signals, and exits (detached). Cancellation is
 * allowed only across the blocking open() (a POSIX cancellation point): its
 * orphaned task is freed by the cleanup handler. */
static void *phasync_oneshot(void *arg)
{
	phasync_task *t = (phasync_task *) arg;

	pthread_cleanup_push(phasync_oneshot_cancelled, t);
	phasync_thread_block_signals();
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	phasync_task_exec(t);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(0);

	phasync_task_signal(t);   /* worker never touches the Zend engine */
	return NULL;
}

static void phasync_pool_child_atfork(void)
{
	/* Runs in the forked child: workers are gone and the mutex may be locked.
	 * Reset everything so the child is clean (and will lazily rebuild a pool
	 * only if it actually uses one). */
	pthread_mutex_init(&phasync_pool_mutex, NULL);
	pthread_cond_init(&phasync_pool_cond, NULL);
	phasync_q_head = phasync_q_tail = NULL;
	phasync_pool_started = 0;
	phasync_pool_stop = 0;
	phasync_pool_n = 0;
}

/* Start the worker pool once. Returns non-zero if at least one worker exists. If
 * none could be created (thread/pid limits), the pool is left unstarted so a later
 * call retries, and the caller runs the task inline instead of queueing it where
 * nothing would ever pick it up. */
static int phasync_pool_ensure(void)
{
	int i, n;
	pthread_attr_t attr;
	if (phasync_pool_started) {
		return 1;
	}
	n = (int) PHASYNC_G(thread_pool_size);
	if (n < 1) n = 1;
	if (n > PHASYNC_POOL_MAX) n = PHASYNC_POOL_MAX;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PHASYNC_WORKER_STACK);

	phasync_pool_stop = 0;
	phasync_pool_n = 0;
	for (i = 0; i < n; i++) {
		if (pthread_create(&phasync_workers[i], &attr, phasync_worker, NULL) != 0) {
			break;
		}
		phasync_pool_n++;
	}
	pthread_attr_destroy(&attr);
	if (phasync_pool_n == 0) {
		return 0;
	}
	phasync_pool_started = 1;
	pthread_atfork(NULL, NULL, phasync_pool_child_atfork);
	return 1;
}

static void phasync_pool_shutdown(void)
{
	int i;
	if (!phasync_pool_started) {
		return;
	}
	pthread_mutex_lock(&phasync_pool_mutex);
	phasync_pool_stop = 1;
	pthread_cond_broadcast(&phasync_pool_cond);
	pthread_mutex_unlock(&phasync_pool_mutex);
	for (i = 0; i < phasync_pool_n; i++) {
		pthread_join(phasync_workers[i], NULL);
	}
	phasync_pool_n = 0;
	phasync_pool_started = 0;
}

static void phasync_pool_submit(phasync_task *t)
{
	pthread_mutex_lock(&phasync_pool_mutex);
	t->next = NULL;
	if (phasync_q_tail) {
		phasync_q_tail->next = t;
	} else {
		phasync_q_head = t;
	}
	phasync_q_tail = t;
	pthread_cond_signal(&phasync_pool_cond);
	pthread_mutex_unlock(&phasync_pool_mutex);
}

/* Submit a task and park the coroutine until a worker has done it. Without a
 * coroutine to park (a fiber being destroyed cannot), a worker, or a slot, run it
 * inline: a pool task always ends. On a cancellation the task is left to its
 * thread and fails with ECANCELED (the exception pending). */
static void phasync_pool_run(phasync_task *t)
{
	phasync_poller *p = PHASYNC_G(scope_top) ? PHASYNC_G(scope_top)->poller : NULL;
	phasync_ledger *l;
	phasync_task *h;

	if (!p || EG(active_fiber) == NULL || phasync_unwinding() || phasync_from_c()
	 || p->fork_gen != phasync_fork_gen || !phasync_pool_ensure() || (t->slot = phasync_get_slot(p)) < 0
	 || (h = phasync_task_own(t)) == NULL) {
		phasync_task_exec(t);
		return;
	}
	phasync_task_chan(h, p);
	GC_ADDREF(&p->std);                  /* the loop may drop the Poller while we wait */
	l = phasync_ledger_cur();
	l->task = h;
	l->task_p = p;
	phasync_pool_submit(h);
	phasync_park(p, h->slot, INFINITY, NULL);
	l->task = NULL;
	phasync_task_finish(h, p, t);
}

/* Like phasync_pool_run, but on a dedicated thread rather than the bounded pool.
 * Used for FIFO open(), whose rendezvous can block indefinitely — a bounded
 * pool would deadlock when reader- and writer-opens outnumber the workers. */
static void phasync_pool_run_dedicated(phasync_task *t)
{
	phasync_poller *p = PHASYNC_G(scope_top) ? PHASYNC_G(scope_top)->poller : NULL;
	phasync_ledger *l;
	phasync_task *h;
	pthread_attr_t attr;
	int rc;

	if (!p || EG(active_fiber) == NULL || phasync_unwinding() || phasync_from_c()
	 || p->fork_gen != phasync_fork_gen || (t->slot = phasync_get_slot(p)) < 0 || (h = phasync_task_own(t)) == NULL) {
		phasync_task_exec(t);   /* no coroutine to park: block inline, as fopen() would */
		return;
	}
	h->dedicated = true;
	phasync_task_chan(h, p);
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PHASYNC_WORKER_STACK);
	rc = pthread_create(&h->thread, &attr, phasync_oneshot, h);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		phasync_chan_release(h->chan);
		phasync_task_free(h);
		phasync_task_exec(t);
		return;
	}
	GC_ADDREF(&p->std);
	l = phasync_ledger_cur();
	l->task = h;
	l->task_p = p;
	phasync_park(p, h->slot, INFINITY, NULL);
	l->task = NULL;
	/* The worker left t->fd as a valid fd, or -1 with t->err set (a real open()
	 * failure, or ECANCELED); the caller distinguishes the two. */
	phasync_task_finish(h, p, t);
}

/* Pool-mode ops: offload the blocking read/write to a worker thread (for regular
 * files, which are not readiness-pollable). Falls back to the original op when
 * there's no scheduler or no fd. */
static ssize_t phasync_pool_read_fd(php_stream *stream, php_socket_t fd, char *buf, size_t count)
{
	phasync_task t;

	memset(&t, 0, sizeof(t));
	t.type = PHASYNC_OP_READ;
	t.fd = fd;
	t.buf = buf;
	t.count = count;
	if (!phasync_inflight_enter(stream)) {
		return -1;
	}
	phasync_pool_run(&t);   /* a close meanwhile waits for it: the read is done */
	phasync_inflight_leave(stream);
	errno = t.err;
	if (t.result == 0) {
		stream->eof = 1;   /* a 0-byte read on a regular file is EOF (as plain
		                    * stdio read does); without this feof() never trips
		                    * and while (!feof($fp)) spins. */
	}
	php_clear_stat_cache(0, NULL, 0);   /* as the native read: atime changed */
	return t.result;
}

static ssize_t phasync_pool_write_fd(php_stream *stream, php_socket_t fd, const char *buf, size_t count)
{
	phasync_task t;

	memset(&t, 0, sizeof(t));
	t.type = PHASYNC_OP_WRITE;
	t.fd = fd;
	t.buf = (char *) buf;
	t.count = count;
	if (!phasync_inflight_enter(stream)) {
		return -1;
	}
	phasync_pool_run(&t);
	phasync_inflight_leave(stream);
	errno = t.err;
	return t.result;
}

static ssize_t phasync_wrapped_read_pool(php_stream *stream, char *buf, size_t count)
{
	php_socket_t fd = phasync_stream_fd(stream);

	/* Outside a scope (or not in a fiber) there is nothing to yield to: be the
	 * native op exactly. */
	if (fd == -1 || !phasync_reading() || EG(active_fiber) == NULL) {
		return PHASYNC_ORIG(stream)->read(stream, buf, count);
	}
	return phasync_pool_read_fd(stream, fd, buf, count);
}

static ssize_t phasync_wrapped_write_pool(php_stream *stream, const char *buf, size_t count)
{
	php_socket_t fd = phasync_stream_fd(stream);

	if (fd == -1 || !phasync_reading() || EG(active_fiber) == NULL) {
		return PHASYNC_ORIG(stream)->write(stream, buf, count);
	}
	return phasync_pool_write_fd(stream, fd, buf, count);
}

