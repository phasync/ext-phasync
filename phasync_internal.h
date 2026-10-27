/* The core extension: its types, module globals and shared prototypes. */
#ifndef PHASYNC_INTERNAL_H
#define PHASYNC_INTERNAL_H

#include "phasync_php.h"
#include "phasync_arginfo.h"
#include "ext/filter/php_filter.h"
#include "main/php_variables.h"


typedef struct {
	int pidfd;          /* process pipe: pidfd of its child (the taint), or -1 */
} phasync_hook_entry;

/* Filesystem functions made cooperative (see "filesystem functions" below). */
typedef enum {
	PHASYNC_FS_WARM,        /* stat the path on the pool, then the original      */
	PHASYNC_FS_WARM_DIR,    /* read the directory on the pool, then the original */
	PHASYNC_FS_WARM_GLOB,   /* WARM_DIR on the pattern's directory               */
	PHASYNC_FS_UNLINK,      /* do it on the pool; the original only on failure   */
	PHASYNC_FS_RMDIR,
	PHASYNC_FS_MKDIR,
	PHASYNC_FS_MKDIR_P,
	PHASYNC_FS_RENAME,
	PHASYNC_FS_FSYNC,       /* fsync(t->fd): fsync() on a file stream             */
	PHASYNC_FS_FDATASYNC
} phasync_fs_op;

static const struct { const char *name; phasync_fs_op op; } phasync_fs_funcs[] = {
	{"stat", PHASYNC_FS_WARM}, {"lstat", PHASYNC_FS_WARM}, {"file_exists", PHASYNC_FS_WARM},
	{"is_file", PHASYNC_FS_WARM}, {"is_dir", PHASYNC_FS_WARM}, {"is_link", PHASYNC_FS_WARM},
	{"is_readable", PHASYNC_FS_WARM}, {"is_writable", PHASYNC_FS_WARM},
	{"is_writeable", PHASYNC_FS_WARM}, {"is_executable", PHASYNC_FS_WARM},
	{"filesize", PHASYNC_FS_WARM}, {"filemtime", PHASYNC_FS_WARM}, {"fileatime", PHASYNC_FS_WARM},
	{"filectime", PHASYNC_FS_WARM}, {"fileperms", PHASYNC_FS_WARM}, {"fileinode", PHASYNC_FS_WARM},
	{"fileowner", PHASYNC_FS_WARM}, {"filegroup", PHASYNC_FS_WARM}, {"filetype", PHASYNC_FS_WARM},
	{"linkinfo", PHASYNC_FS_WARM}, {"readlink", PHASYNC_FS_WARM}, {"realpath", PHASYNC_FS_WARM},
	{"scandir", PHASYNC_FS_WARM_DIR}, {"opendir", PHASYNC_FS_WARM_DIR}, {"dir", PHASYNC_FS_WARM_DIR},
	{"glob", PHASYNC_FS_WARM_GLOB},
	{"file_get_contents", PHASYNC_FS_WARM}, {"file_put_contents", PHASYNC_FS_WARM},
	{"file", PHASYNC_FS_WARM}, {"readfile", PHASYNC_FS_WARM}, {"copy", PHASYNC_FS_WARM},
	{"md5_file", PHASYNC_FS_WARM}, {"sha1_file", PHASYNC_FS_WARM},
	{"touch", PHASYNC_FS_WARM}, {"chmod", PHASYNC_FS_WARM}, {"chown", PHASYNC_FS_WARM},
	{"chgrp", PHASYNC_FS_WARM}, {"lchown", PHASYNC_FS_WARM}, {"lchgrp", PHASYNC_FS_WARM},
	{"link", PHASYNC_FS_WARM}, {"symlink", PHASYNC_FS_WARM}, {"tempnam", PHASYNC_FS_WARM},
	{"disk_free_space", PHASYNC_FS_WARM}, {"diskfreespace", PHASYNC_FS_WARM},
	{"disk_total_space", PHASYNC_FS_WARM},
	{"unlink", PHASYNC_FS_UNLINK}, {"rmdir", PHASYNC_FS_RMDIR}, {"mkdir", PHASYNC_FS_MKDIR},
	{"rename", PHASYNC_FS_RENAME},
};
#define PHASYNC_FS_NFUNCS (sizeof(phasync_fs_funcs) / sizeof(phasync_fs_funcs[0]))

#define PHASYNC_FS_OFFLOAD_NETWORK 0   /* phasync.fs_offload: network/FUSE mounts only */
#define PHASYNC_FS_OFFLOAD_ALL     1
#define PHASYNC_FS_OFFLOAD_NONE    2

/* How a regular file's reads run in a coroutine. */
#define PHASYNC_FS_INLINE 0   /* natively: not in a coroutine, or can't tell a cache hit */
#define PHASYNC_FS_NOWAIT 1   /* inline from the page cache, on the pool when it would block */
#define PHASYNC_FS_POOL   2   /* always on the pool */

typedef struct {
	char  *path;
	size_t len;
	dev_t  dev;
	bool   slow;                  /* network/FUSE: metadata and file data on the pool */
	bool   pool;                  /* file data on the pool: slow, or in phasync.fs_offload_types */
} phasync_mount;

/* This thread's children just before an exec-family call spawns one. */
#define PHASYNC_MAX_CHILDREN 256
typedef struct {
	pid_t before[PHASYNC_MAX_CHILDREN];
	int   n;                /* -1: /proc children unavailable */
} phasync_spawn;

/* Wrapped ops with the original embedded right after it, so the original is
 * recoverable from any stream carrying these ops (including accepted sockets,
 * which inherit the listener's ops pointer without going through our wrap path). */
typedef struct {
	php_stream_ops ops;             /* MUST be first member */
	const php_stream_ops *orig;
} phasync_wops;

/* stream->ops points at the .ops member (first) of a phasync_wops. */
#define PHASYNC_ORIG(stream) (((phasync_wops *) (stream)->ops)->orig)

typedef enum {
	PHASYNC_MODE_RAW  = 0,   /* raw read/write on the fd (sockets, pipes)     */
	PHASYNC_MODE_TLS  = 1,   /* delegate to the original op (SSL_read/write)  */
	PHASYNC_MODE_POOL = 2    /* offload to the thread pool (regular files)    */
} phasync_mode;

/* What the hooked operations wait through while code runs under a managed fiber
 * (suspension.c): the request's poller. */
struct phasync_poller;
struct phasync_susp;

typedef struct phasync_scope {
	struct phasync_poller *poller;
} phasync_scope;


struct phasync_preempt;

ZEND_BEGIN_MODULE_GLOBALS(phasync)
	struct phasync_preempt *preempt; /* Suspension::setPreemptInterval()'s timer, NULL = none */
	zval *preempt_due;            /* phasync\ext\__PREEMPT_DUE's value: true = a checkpoint calls */
	struct phasync_susp **susp;   /* the Suspension pool; an index is a wait's slot */
	uint32_t *susp_spare;         /* a stack of indexes of spares: wait over, only the pool holds them */
	uint32_t nsusp, capsusp, nspare;
	struct phasync_poller *proc_poller; /* the request's poller, created on first use */
	phasync_scope proc_scope;
	bool managing;                /* Suspension::manage() was called this request */
	php_stream_transport_factory orig_tcp;
	php_stream_transport_factory orig_unix;
	php_stream_transport_factory orig_udp;
	php_stream_transport_factory orig_udg;
	php_stream_transport_factory orig_ssl;
	void (*orig_proc_open)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_sleep)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_usleep)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_header_register_callback)(INTERNAL_FUNCTION_PARAMETERS);
	zend_function *header_cb;   /* the registered header callback: never suspends (poller.c) */
	void (*orig_time_nanosleep)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_time_sleep_until)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_gethostbyname)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_gethostbynamel)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_gethostbyaddr)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_dns_check_record)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_dns_get_record)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_dns_get_mx)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_fopen)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_fclose)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_stream_socket_pair)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_stream_select)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_socket_select)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_popen)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_exec)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_system)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_passthru)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_shell_exec)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_proc_close)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_pclose)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_pcntl_waitpid)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_pcntl_wait)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_socket_connect)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_socket_close)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_curl_multi_select)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_sem_acquire)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_curl_exec)(INTERNAL_FUNCTION_PARAMETERS);
	void (*orig_msg_receive)(INTERNAL_FUNCTION_PARAMETERS);
	zif_handler orig_sock[8];
	HashTable sock_hooks;         /* (uintptr_t)function_name -> index in phasync_sock_funcs */
	HashTable conn_hooks;         /* functions on a connection routed through phasync_conn_call() */
	HashTable conn_busy;          /* connection -> the fiber context in a call on it */
	phasync_spawn *spawn;         /* armed by an exec-family call until its pipe is seen */
	bool ub_writing;              /* a fiber is suspended inside an echo (CLI stdout) */
	zif_handler orig_fs[PHASYNC_FS_NFUNCS];
	HashTable fs_hooks;           /* (uintptr_t)function_name -> index in phasync_fs_funcs */
	int fs_offload;               /* INI: phasync.fs_offload */
	int mountinfo_fd;             /* /proc/self/mountinfo, kept open to poll for changes */
	uint64_t mounts_sig;          /* elsewhere: hash of the mount table last loaded; 0: none */
	time_t mounts_checked;        /* monotonic second of the last poll of it */
	char *fs_offload_types;       /* INI: phasync.fs_offload_types */
	php_stream *fs_last;          /* last stdio stream whose reads were decided, and */
	int fs_last_mode;             /* how (PHASYNC_FS_*): its later reads skip deciding;
	                               * cleared when it closes */
	phasync_mount *mounts;
	int nmounts;
	bool any_slow_mount;
	bool any_pool_mount;
	dev_t *nowait_off;            /* devices whose files refuse RWF_NOWAIT (ZFS ...) */
	int nnowait_off;
	dev_t *pipe_nowait_off;       /* issue #42: devices whose PIPES/FIFOs refuse RWF_NOWAIT */
	int npipe_nowait_off;         /* (independent of nowait_off: a plain file and a FIFO on */
	                              /* the very same device have been seen to differ) */
	int select_depth;             /* >0 while our override probes the original select */
	zend_long thread_pool_size;   /* INI: phasync.thread_pool_size */
	bool hooks_installed;         /* transports + fn overrides physically in place */
	HashTable hooked;             /* (uintptr_t)stream    -> phasync_hook_entry* */
	HashTable inflight;           /* (uintptr_t)stream    -> phasync_inflight*   */
	HashTable ledgers;            /* (uintptr_t)zend_fiber_context -> phasync_ledger* */
	uint32_t holds;               /* closers held alive while they wait (#21)       */
	HashTable wrapped_ops_cache;  /* (uintptr_t)orig_ops  -> php_stream_ops*     */
	uint64_t sw_pipe_nowait;      /* phasync_wrapped_read/write took the preadv2()/pwritev2() RWF_NOWAIT path */
	uint64_t sw_pipe_fcntl_fallback; /* ...fell back to the O_NONBLOCK toggle (no RWF_NOWAIT support) */
ZEND_END_MODULE_GLOBALS(phasync)

ZEND_DECLARE_MODULE_GLOBALS(phasync)

#ifdef ZTS
# define PHASYNC_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(phasync, v)
#else
# define PHASYNC_G(v) (phasync_globals.v)
#endif

/* Waits suspend the nearest managed fiber (suspension.c). Outside one, hooked I/O
 * falls through to the original behaviour: these return 0, and the wait direction
 * (PHASYNC_READ / PHASYNC_WRITE) under one. */
#define PHASYNC_READ   1
#define PHASYNC_WRITE  2

static phasync_scope *phasync_scope_top(void);

static zend_always_inline int phasync_reading(void)
{
	return phasync_scope_top() ? PHASYNC_READ : 0;
}
static zend_always_inline int phasync_writing(void)
{
	return phasync_scope_top() ? PHASYNC_WRITE : 0;
}

/* ---- types shared across more than one of this extension's own source
 * files (moved here verbatim from their original spot in the pre-split
 * phasync.c); every one of them is still `static`-visibility-free (this
 * extension builds as a single translation unit: phasync.c #includes every
 * other .c file listed here, so nothing needs cross-TU linkage) ---- */

/* Tri-state result of a wait. */
#define PHASYNC_WAIT_READY    0    /* ready (or closed/failed) -> retry the op       */
#define PHASYNC_WAIT_TIMEOUT  1    /* native timeout ran out   -> finish like native */
#define PHASYNC_WAIT_ERROR    (-1) /* exception pending (a cancellation) -> propagate */
#define PHASYNC_WAIT_CLOSED   2    /* another coroutine closed the stream (EBADF)    */

typedef struct {
	pthread_mutex_t mutex;
	zend_long      *queue;    /* slots of finished tasks, for poll()            */
	size_t          len, cap;
	int             evfd, evfd_w; /* read end, write end (the same eventfd on Linux) */
	int             refs;     /* the poller + each running task; under mutex    */
} phasync_chan;

typedef struct {
	php_socket_t fd;
	php_stream  *stream;      /* owner; NULL for a bare descriptor               */
	zend_long    slot[2];     /* parked reader [0] and writer [1]; -1 = nobody   */
	zend_long    sid[2];      /* hooked: the slot each direction reuses; -1 = none yet */
	uint32_t     events;      /* hooked: the interest registered (level-triggered) */
	bool         added;       /* in the epoll set (as far as we know)            */
	bool         transient;   /* for the current wait only (a bare descriptor)   */
	bool         hooked;      /* the stream's close op deregisters it            */
	uint8_t      watch;       /* bit idx: slot[idx] is a stream_select() close watch, not a wait */
} phasync_reg;
/* slot[i] waits for readiness (a close watch only waits for a close). */
#define PHASYNC_REG_WAITS(r, i) ((r)->slot[i] >= 0 && !((r)->watch & (1 << (i))))

typedef struct phasync_poller {
	int           epfd;
	unsigned      fork_gen;   /* epoll and eventfd are shared with a fork()ed child */
	phasync_chan *chan;
	HashTable     regs;       /* fd -> phasync_reg*                              */
	zend_long     armed;      /* slots parked on a registration (poll(0) skips epoll without) */
	bool          select_mode;    /* below threshold: select() instead of epoll, no epoll_ctl */
	void         *sel_rfds, *sel_wfds;   /* malloc'd past FD_SETSIZE; bits set by hand (#define
	                                       * below), never through libc's FORTIFY-checked FD_SET */
	size_t        sel_cap;        /* bytes allocated for each of sel_rfds/sel_wfds */
	struct phasync_poller *next;   /* the live pollers (for the close hooks)     */
	uint32_t      refs;     /* the request + each waiter (a fork()ed child's old one: its waiters) */
} phasync_poller;

typedef struct {
	phasync_poller *wait_p;       /* parked on a registration of it (a reference): */
	php_stream     *wait_stream;
	php_socket_t    wait_fd;
	int             wait_idx, wait_dupfd;
	zend_long       wait_slot;
	void           *inflight;     /* inside an op on it: one of its waiters (#14)  */
	php_stream     *closing;      /* its closer, parked in closing_p (a reference) */
	phasync_poller *closing_p;
	struct phasync_task *task;    /* a pool operation reporting to task_p (a reference) */
	phasync_poller *task_p;
} phasync_ledger;

typedef struct {
	uint32_t        waiters;      /* coroutines suspended in an op on the stream */
	bool            closing;      /* a close woke them; ops on it fail (EBADF)   */
	phasync_poller *closer;       /* where the closer is parked (NULL: none), and its slot */
	zend_long       closer_slot;
	zend_object    *hold;         /* the closer's fiber, held while it waits     */
} phasync_inflight;

typedef enum {
	PHASYNC_OP_READ,
	PHASYNC_OP_WRITE,
	PHASYNC_OP_GETHOSTBYNAME,
	PHASYNC_OP_GETADDRINFO,
	PHASYNC_OP_NAMEINFO,
	PHASYNC_OP_DNSQUERY,
	PHASYNC_OP_FS,             /* filesystem metadata/namespace op (phasync_fs_op) */
	PHASYNC_OP_OPEN,           /* blocking open() (FIFO rendezvous), own thread   */
	PHASYNC_OP_CALL            /* call(arg): waited for to the end, even when cancelled */
} phasync_op_type;

typedef struct phasync_task {
	phasync_op_type type;
	int          fd;          /* READ/WRITE; OPEN: resulting fd                 */
	char        *buf;         /* READ: dest (caller's buffer); WRITE: source   */
	size_t       count;
	ssize_t      result;
	int          err;
	const char  *host;        /* GETHOSTBYNAME input (plain C string)          */
	char         hostresult[NI_MAXHOST]; /* NAMEINFO result                       */
	int          hostok;
	struct in_addr addrs[64]; /* GETHOSTBYNAME: h_addr_list (IPv4)             */
	int          naddrs;
	struct sockaddr_storage sa; /* NAMEINFO input                              */
	socklen_t    salen;
	int          qtype;       /* DNSQUERY: record type; buf/count = answer buffer */
	struct addrinfo  hints;   /* GETADDRINFO input                             */
	char         serv[NI_MAXSERV]; /* GETADDRINFO service (a port), "" for none */
	struct addrinfo *ai;      /* GETADDRINFO result (caller freeaddrinfo()s)   */
	char         path[PATH_MAX]; /* OPEN: path (own copy, worker-stable)        */
	int          oflags;      /* OPEN: open(2) flags                           */
	int          omode;       /* OPEN: open(2) mode; FS: mkdir(2) mode          */
	phasync_fs_op fsop;       /* FS: which operation                           */
	void       (*call)(void *); /* CALL: the function and its argument           */
	void        *arg;
	char         path2[PATH_MAX]; /* FS rename: destination                     */
	zend_long    slot;        /* the waiter's slot, queued for poll() when done  */
	void        *chan;        /* its poller's completion channel (a reference)   */
	int          done;        /* set (under the channel's mutex) once finished   */
	int          orphaned;    /* its coroutine is gone: the thread frees it (ditto) */
	int          ownfd;       /* its own duplicate of fd (READ/WRITE/fsync), or -1 */
	bool         dedicated;   /* on its own thread: */
	pthread_t    thread;
	struct phasync_task *next;
} phasync_task;



/* ---- forward declarations: a static function or global defined in one of
 * this extension's own source files, used in a file this single-TU build
 * #includes earlier (see phasync.c's own include list and comment) ---- */

static php_socket_t phasync_stream_fd(php_stream *stream);
static bool phasync_cannot_suspend(void);
static bool phasync_unwinding(void);
static bool phasync_op_array_no_preempt(const zend_op_array *op_array);
static double phasync_now(void);
static zend_long phasync_get_slot(struct phasync_poller *p);
static int phasync_park(struct phasync_poller *p, zend_long slot, double timeout);
static void phasync_poller_release(phasync_poller *p);
static int phasync_susp_sleep(double seconds);
static void phasync_holds_release(void);
static ssize_t phasync_wrapped_read_pool(php_stream *stream, char *buf, size_t count);
static ssize_t phasync_wrapped_write_pool(php_stream *stream, const char *buf, size_t count);
static void phasync_pool_run(phasync_task *t);
static void phasync_pool_run_dedicated(phasync_task *t);
static ssize_t phasync_pool_read_fd(php_stream *stream, php_socket_t fd, char *buf, size_t count);
static ssize_t phasync_pool_write_fd(php_stream *stream, php_socket_t fd, const char *buf, size_t count);
static ZEND_NAMED_FUNCTION(phasync_stream_socket_pair_override);
static ZEND_NAMED_FUNCTION(phasync_fs_override);
static ZEND_NAMED_FUNCTION(phasync_fclose_override);
static ZEND_NAMED_FUNCTION(phasync_pclose_override);




#endif /* PHASYNC_INTERNAL_H */
