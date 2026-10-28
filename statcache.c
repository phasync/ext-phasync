/* The evented stat cache (phasync.stat_cache; docs/stat-cache.md): file_exists(), is_file(),
 * filemtime() and the rest of the stat family answered from remembered lstat()/access() results,
 * kept valid by inotify instead of a syscall per call. Linux only.
 *
 * A result is kept for a path whose every directory, from the root down, is watched (one watch
 * per directory, shared by every path under it): a change to an entry of a watched directory
 * drops that entry's result (and the directory's own); a change to a watched directory itself
 * (renamed, deleted, chmod'ed, replaced) or a queue overflow drops everything. Watches are added
 * with IN_DONT_FOLLOW|IN_ONLYDIR, so a watched path holds no symlink and ".." in a path is
 * resolved lexically. The inotify fd is O_ASYNC: the kernel raises SIGIO, whose handler only
 * sets a flag, as it queues an event, so a lookup costs no syscall while nothing changed and
 * sees every change queued before it, this process's own writes included. Directories on
 * filesystems inotify cannot vouch for (NFS, SMB, FUSE, 9p, ...) are never cached. */

#ifdef __linux__
#include <sys/inotify.h>
#include <sys/vfs.h>
#if __has_include(<valgrind/valgrind.h>)
# include <valgrind/valgrind.h>
#endif

#define PHASYNC_SC_MASK (IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO \
	| IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW)
#define PHASYNC_SC_MAX_PATHS 65536   /* results + other spellings, then everything is dropped */

typedef struct {
	bool exists;          /* lstat() succeeded */
	uint8_t known, ok;    /* access() R_OK/W_OK/X_OK asked / succeeded, as R_OK|W_OK|X_OK bits */
	uint64_t serial;      /* new for each result: a directory's changes whenever an entry comes or goes */
	zend_stat_t st;       /* lstat()'s result */
} phasync_sc_rec;

enum { PHASYNC_SC_EXISTS, PHASYNC_SC_ACCESS, PHASYNC_SC_STATQ, PHASYNC_SC_STAT, PHASYNC_SC_LSTATQ, PHASYNC_SC_LSTAT,
	PHASYNC_SC_REALPATH, PHASYNC_SC_CLEAR };

static const struct { const char *name; uint8_t q, mode; } phasync_sc_fns[] = {
	{ "file_exists", PHASYNC_SC_EXISTS, 0 }, { "is_readable", PHASYNC_SC_ACCESS, R_OK },
	{ "is_writable", PHASYNC_SC_ACCESS, W_OK }, { "is_writeable", PHASYNC_SC_ACCESS, W_OK },
	{ "is_executable", PHASYNC_SC_ACCESS, X_OK }, { "is_file", PHASYNC_SC_STATQ, 0 }, { "is_dir", PHASYNC_SC_STATQ, 0 },
	{ "filemtime", PHASYNC_SC_STAT, 0 }, { "filesize", PHASYNC_SC_STAT, 0 }, { "filectime", PHASYNC_SC_STAT, 0 },
	{ "fileperms", PHASYNC_SC_STAT, 0 }, { "fileinode", PHASYNC_SC_STAT, 0 }, { "fileowner", PHASYNC_SC_STAT, 0 },
	{ "filegroup", PHASYNC_SC_STAT, 0 }, { "stat", PHASYNC_SC_STAT, 0 }, { "is_link", PHASYNC_SC_LSTATQ, 0 },
	{ "lstat", PHASYNC_SC_LSTAT, 0 }, { "filetype", PHASYNC_SC_LSTAT, 0 }, { "realpath", PHASYNC_SC_REALPATH, 0 },
	{ "clearstatcache", PHASYNC_SC_CLEAR, 0 },
};
#define PHASYNC_SC_NFNS (sizeof phasync_sc_fns / sizeof phasync_sc_fns[0])

static zif_handler phasync_sc_orig[PHASYNC_SC_NFNS];
static bool phasync_sc_on, phasync_sc_untrusted, phasync_sc_valgrind;   /* valgrind delivers signals late: read every time */
static int phasync_sc_fd = -1;
static volatile sig_atomic_t phasync_sc_dirty;
static zend_long phasync_sc_cap;
static HashTable phasync_sc_dirs;    /* canonical directory -> its wd, or -1: not cacheable (symlink, untrusted fs, ...) */
static HashTable phasync_sc_wds;     /* wd -> directory (zend_string) */
static HashTable phasync_sc_recs;    /* canonical path -> phasync_sc_rec */
static HashTable phasync_sc_inherited;   /* a fork()ed child: the parent's records, for their serials */
static HashTable phasync_sc_alias;   /* other spelling -> canonical path (IS_STRING), or the ancestor that is missing or no directory (IS_PTR) */
static uint64_t phasync_sc_hits, phasync_sc_misses, phasync_sc_fallbacks, phasync_sc_flushes, phasync_sc_events, phasync_sc_serial;

static void phasync_sc_sigio(int sig)
{
	(void) sig;
	phasync_sc_dirty = 1;
}

static void phasync_sc_flush(void)
{
	if (phasync_sc_fd >= 0) {
		close(phasync_sc_fd);
		phasync_sc_fd = -1;
	}
	zend_hash_clean(&phasync_sc_dirs);
	zend_hash_clean(&phasync_sc_wds);
	zend_hash_clean(&phasync_sc_recs);
	zend_hash_clean(&phasync_sc_alias);
	phasync_sc_flushes++;
}

/* One event; false: everything must go */
static bool phasync_sc_event(const struct inotify_event *ev)
{
	zend_string *d;
	char t[MAXPATHLEN];
	size_t tl;

	phasync_sc_events++;
	if (ev->mask & IN_Q_OVERFLOW) {
		return false;
	}
	if (!(d = zend_hash_index_find_ptr(&phasync_sc_wds, ev->wd))) {
		return true;   /* a watch given back */
	}
	if ((ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) || !ev->len) {
		return false;  /* the directory itself */
	}
	tl = ZSTR_LEN(d) == 1 ? 0 : ZSTR_LEN(d);
	memcpy(t, ZSTR_VAL(d), tl);
	t[tl++] = '/';
	tl += strlcpy(t + tl, ev->name, sizeof t - tl);
	if (zend_hash_str_exists(&phasync_sc_dirs, t, tl)) {
		return false;  /* a directory we resolve through */
	}
	zend_hash_str_del(&phasync_sc_recs, t, tl);
	if (ev->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) {
		zend_hash_del(&phasync_sc_recs, d);
	}
	return true;
}

static void phasync_sc_drain(void)
{
	char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;

	phasync_sc_dirty = 0;
	while (phasync_sc_fd >= 0) {
		if ((n = read(phasync_sc_fd, buf, sizeof buf)) <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return;
		}
		for (char *q = buf; q < buf + n; ) {
			const struct inotify_event *ev = (const struct inotify_event *) q;
			q += sizeof *ev + ev->len;
			if (!phasync_sc_event(ev)) {
				phasync_sc_flush();
				return;
			}
		}
	}
}

/* Can inotify see every change to this filesystem? Not on network, cluster and FUSE ones.
 * 1 yes, 0 no, -1 statfs() failed (errno) */
static int phasync_sc_trusted(const char *dir)
{
	struct statfs fs;

	if (statfs(dir, &fs) != 0) {
		return -1;
	}
	if (phasync_sc_untrusted) {
		return 0;
	}
	switch ((unsigned long) fs.f_type) {
	case 0xEF53:      /* ext2/3/4 */
	case 0x58465342:  /* xfs */
	case 0x9123683E:  /* btrfs */
	case 0x2FC12FC1:  /* zfs */
	case 0x01021994:  /* tmpfs */
	case 0x858458F6:  /* ramfs */
	case 0xF2F52010:  /* f2fs */
	case 0xCA451A4E:  /* bcachefs */
	case 0x794C7630:  /* overlayfs: changes made through it */
	case 0x73717368:  /* squashfs */
	case 0xE0F5E1E2:  /* erofs */
		return 1;
	}
	return 0;
}

static phasync_sc_rec *phasync_sc_fill(const char *p, size_t len)
{
	phasync_sc_rec *r = pemalloc(sizeof *r, 1);

	r->known = r->ok = 0;
	r->serial = ++phasync_sc_serial;
	if (!(r->exists = lstat(p, &r->st) == 0)) {
		memset(&r->st, 0, sizeof r->st);
	}
	if (zend_hash_num_elements(&phasync_sc_inherited)) {
		phasync_sc_rec *o = zend_hash_str_find_ptr(&phasync_sc_inherited, p, len);
		/* unchanged since the parent's lstat(): its serial, so the memo's inherited keys still match */
		if (o && o->exists == r->exists && o->st.st_dev == r->st.st_dev && o->st.st_ino == r->st.st_ino
		 && o->st.st_mode == r->st.st_mode && o->st.st_size == r->st.st_size
		 && o->st.st_mtim.tv_sec == r->st.st_mtim.tv_sec && o->st.st_mtim.tv_nsec == r->st.st_mtim.tv_nsec
		 && o->st.st_ctim.tv_sec == r->st.st_ctim.tv_sec && o->st.st_ctim.tv_nsec == r->st.st_ctim.tv_nsec) {
			r->serial = o->serial;
		}
	}
	return zend_hash_str_update_ptr(&phasync_sc_recs, p, len, r);
}

static bool phasync_sc_init(void)
{
	struct f_owner_ex owner = { F_OWNER_TID, (pid_t) syscall(SYS_gettid) };

	if ((phasync_sc_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) < 0) {
		phasync_sc_on = false;   /* max_user_instances */
		return false;
	}
	fcntl(phasync_sc_fd, F_SETOWN_EX, &owner);
	fcntl(phasync_sc_fd, F_SETFL, O_NONBLOCK | O_ASYNC);
	return true;
}

/* A canonical directory whose parent is watched: 1 watched, 0 not cacheable, -1 missing or no directory */
static int phasync_sc_dir(const char *d, size_t len)
{
	zval *z = zend_hash_str_find(&phasync_sc_dirs, d, len), v;
	phasync_sc_rec *r;
	zend_string *k;
	int wd;

	if (z) {
		return Z_LVAL_P(z) >= 0;
	}
	if ((r = zend_hash_str_find_ptr(&phasync_sc_recs, d, len)) && (!r->exists || (!S_ISDIR(r->st.st_mode) && !S_ISLNK(r->st.st_mode)))) {
		return -1;
	}
	ZVAL_LONG(&v, -1);
	if ((wd = phasync_sc_trusted(d)) <= 0) {
		if (wd < 0 && (errno == ENOENT || errno == ENOTDIR)) {
			goto missing;
		}
		zend_hash_str_add(&phasync_sc_dirs, d, len, &v);
		return 0;
	}
	if ((wd = inotify_add_watch(phasync_sc_fd, d, PHASYNC_SC_MASK)) < 0) {
		if (errno == ENOENT || errno == ENOTDIR) {
			goto missing;
		}
		zend_hash_str_add(&phasync_sc_dirs, d, len, &v);   /* EACCES, ENOSPC (max_user_watches), ... */
		return 0;
	}
	if (zend_hash_index_exists(&phasync_sc_wds, wd)) {
		zend_hash_str_add(&phasync_sc_dirs, d, len, &v);   /* bind-mounted twice: one wd, two paths */
		return 0;
	}
	k = zend_string_init(d, len, 1);
	ZVAL_LONG(&v, wd);
	zend_hash_add(&phasync_sc_dirs, k, &v);
	zend_hash_index_add_new_ptr(&phasync_sc_wds, wd, k);   /* takes the reference */
	return 1;
missing:
	r = phasync_sc_fill(d, len);
	if (r->exists && S_ISLNK(r->st.st_mode)) {
		zend_hash_str_add(&phasync_sc_dirs, d, len, &v);
		return 0;
	}
	return r->exists && S_ISDIR(r->st.st_mode) ? 0 : -1;   /* a directory: created just now */
}

/* An absolute path's result: 1 (*rec), 0 not cacheable, -1 an ancestor is missing or no directory */
static int phasync_sc_resolve(zend_string *path, phasync_sc_rec **rec)
{
	const char *s = ZSTR_VAL(path) + 1, *end = ZSTR_VAL(path) + ZSTR_LEN(path), *e;
	char buf[MAXPATHLEN];
	size_t n = 0;
	int r;
	zval a;

	if (ZSTR_LEN(path) < 2 || end[-1] == '/' || (phasync_sc_fd < 0 && !phasync_sc_init())) {
		return 0;
	}
	if ((r = phasync_sc_dir("/", 1)) != 1) {
		return 0;
	}
	for (;; s = e + 1) {
		size_t cl;

		e = memchr(s, '/', end - s);
		cl = (e ? e : end) - s;
		if (cl == 0 || (cl == 1 && s[0] == '.') || (cl == 2 && s[0] == '.' && s[1] == '.')) {
			if (!e) {
				return 0;  /* ends in "/." or "/.." */
			}
			if (cl == 2) {
				while (n > 0 && buf[--n] != '/');   /* buf is a watched directory: its parent */
			}
			continue;
		}
		if (n + 1 + cl >= sizeof buf) {
			return 0;
		}
		buf[n] = '/';
		memcpy(buf + n + 1, s, cl);
		n += 1 + cl;
		buf[n] = '\0';
		if (!e) {
			break;
		}
		if ((r = phasync_sc_dir(buf, n)) != 1) {
			if (r < 0) {
				ZVAL_PTR(&a, zend_string_init(buf, n, 1));
				zend_hash_str_update(&phasync_sc_alias, ZSTR_VAL(path), ZSTR_LEN(path), &a);
			}
			return r;
		}
	}
	*rec = phasync_sc_fill(buf, n);
	if ((*rec)->exists && S_ISDIR((*rec)->st.st_mode)) {
		if (phasync_sc_dir(buf, n) != 1) {   /* its own stat changes with its entries: watched too */
			zend_hash_str_del(&phasync_sc_recs, buf, n);
			return 0;
		}
		*rec = phasync_sc_fill(buf, n);      /* again, now that changes are seen */
	}
	if (n != ZSTR_LEN(path) || memcmp(buf, ZSTR_VAL(path), n)) {
		ZVAL_STR(&a, zend_string_init(buf, n, 1));
		zend_hash_str_update(&phasync_sc_alias, ZSTR_VAL(path), ZSTR_LEN(path), &a);
	}
	return 1;
}

static int phasync_sc_get(zend_string *path, phasync_sc_rec **rec)
{
	zval *a;
	int r;

	if (UNEXPECTED(phasync_sc_dirty || phasync_sc_valgrind)) {
		phasync_sc_drain();
	}
	if ((*rec = zend_hash_find_ptr(&phasync_sc_recs, path))) {
		phasync_sc_hits++;
		return 1;
	}
	if ((a = zend_hash_find(&phasync_sc_alias, path))) {
		if (Z_TYPE_P(a) == IS_STRING) {
			if ((*rec = zend_hash_find_ptr(&phasync_sc_recs, Z_STR_P(a)))) {
				phasync_sc_hits++;
				return 1;
			}
		} else if ((*rec = zend_hash_find_ptr(&phasync_sc_recs, (zend_string *) Z_PTR_P(a)))
		 && (!(*rec)->exists || (!S_ISDIR((*rec)->st.st_mode) && !S_ISLNK((*rec)->st.st_mode)))) {
			phasync_sc_hits++;
			return -1;
		}
	}
	if ((zend_long) zend_hash_num_elements(&phasync_sc_wds) >= phasync_sc_cap
	 || zend_hash_num_elements(&phasync_sc_recs) + zend_hash_num_elements(&phasync_sc_alias) >= PHASYNC_SC_MAX_PATHS) {
		phasync_sc_flush();
	}
	if ((r = phasync_sc_resolve(path, rec))) {
		phasync_sc_misses++;
	} else {
		phasync_sc_fallbacks++;
	}
	return r;
}

/* stat() for the memo's keys; *serial: the result's (0: not cached), which changes when a
 * directory's entries or a file's content change, even within one timestamp tick */
static int phasync_sc_stat(const char *path, zend_stat_t *st, uint64_t *serial)
{
	*serial = 0;
	if (phasync_sc_on && path[0] == '/') {
		zend_string *p = zend_string_init(path, strlen(path), 0);
		phasync_sc_rec *rec;
		int r = phasync_sc_get(p, &rec);

		zend_string_release(p);
		if (r < 0 || (r > 0 && !rec->exists)) {
			errno = ENOENT;
			return -1;
		}
		if (r > 0 && !S_ISLNK(rec->st.st_mode)) {
			*st = rec->st;
			*serial = rec->serial;
			return 0;
		}
	}
	return stat(path, st);
}

/* Hand a result to php_stat() through its own one-entry cache, which it consults first */
static void phasync_sc_inject(zend_string *path, const zend_stat_t *st, bool link)
{
	zend_string **cur = link ? &BG(CurrentLStatFile) : &BG(CurrentStatFile);

	if (*cur != path) {
		if (*cur) {
			zend_string_release(*cur);
		}
		*cur = zend_string_copy(path);
	}
	memcpy(link ? &BG(lssb).sb : &BG(ssb).sb, st, sizeof *st);
}

static void phasync_sc_call(INTERNAL_FUNCTION_PARAMETERS, int i)
{
	zval *arg = ZEND_CALL_ARG(execute_data, 1);
	uint8_t q = phasync_sc_fns[i].q;
	phasync_sc_rec *rec = NULL;
	zend_string *p;
	bool fail;
	int r;

	if (q == PHASYNC_SC_CLEAR) {
		phasync_sc_orig[i](INTERNAL_FUNCTION_PARAM_PASSTHRU);
		if (phasync_sc_on) {
			phasync_sc_drain();   /* a sync point: every change queued so far is seen */
			if (ZEND_NUM_ARGS() == 2 && Z_TYPE_P(ZEND_CALL_ARG(execute_data, 2)) == IS_STRING) {
				zval *a = zend_hash_find(&phasync_sc_alias, Z_STR_P(ZEND_CALL_ARG(execute_data, 2)));
				if (a && Z_TYPE_P(a) == IS_STRING) {
					zend_hash_del(&phasync_sc_recs, Z_STR_P(a));
				}
				zend_hash_del(&phasync_sc_recs, Z_STR_P(ZEND_CALL_ARG(execute_data, 2)));
			}
		}
		return;
	}
	if (!phasync_sc_on || ZEND_NUM_ARGS() != 1 || Z_TYPE_P(arg) != IS_STRING) {
		goto orig;
	}
	p = Z_STR_P(arg);
	if (ZSTR_VAL(p)[0] != '/' || zend_str_has_nul_byte(p)
	 || php_stream_locate_url_wrapper(ZSTR_VAL(p), NULL, 0) != &php_plain_files_wrapper
	 || (PG(open_basedir) && *PG(open_basedir) && php_check_open_basedir_ex(ZSTR_VAL(p), 0))) {
		goto orig;   /* relative, another wrapper, or open_basedir says no: PHP's own way, warnings included */
	}
	if (q >= PHASYNC_SC_STATQ && q <= PHASYNC_SC_LSTAT) {
		zend_string *cur = q >= PHASYNC_SC_LSTATQ ? BG(CurrentLStatFile) : BG(CurrentStatFile);
		if (cur && zend_string_equals(cur, p)) {
			goto orig;   /* PHP's own one-entry cache answers, stale as it may be, as without us */
		}
	}
	if (!(r = phasync_sc_get(p, &rec))) {
		goto orig;
	}
	fail = r < 0 || !rec->exists;
	if (!fail && S_ISLNK(rec->st.st_mode) && q != PHASYNC_SC_LSTATQ && q != PHASYNC_SC_LSTAT) {
		goto orig;   /* followed: the target's directory is not watched */
	}
	switch (q) {
	case PHASYNC_SC_EXISTS:
		RETURN_BOOL(!fail);
	case PHASYNC_SC_ACCESS:
		if (fail) {
			RETURN_FALSE;
		}
		if (!(rec->known & phasync_sc_fns[i].mode)) {
			rec->known |= phasync_sc_fns[i].mode;
			if (access(ZSTR_VAL(p), phasync_sc_fns[i].mode) == 0) {
				rec->ok |= phasync_sc_fns[i].mode;
			}
		}
		RETURN_BOOL(rec->ok & phasync_sc_fns[i].mode);
	case PHASYNC_SC_STATQ:
	case PHASYNC_SC_LSTATQ:
		if (fail) {
			RETURN_FALSE;
		}
		ZEND_FALLTHROUGH;
	case PHASYNC_SC_STAT:
	case PHASYNC_SC_LSTAT:
		if (!fail) {
			phasync_sc_inject(p, &rec->st, q == PHASYNC_SC_LSTATQ || q == PHASYNC_SC_LSTAT);
		}
		break;   /* a failure: PHP's own warning */
	case PHASYNC_SC_REALPATH:
		if (fail && !strstr(ZSTR_VAL(p), "/.") && !strstr(ZSTR_VAL(p), "//") && !realpath_cache_lookup(ZSTR_VAL(p), ZSTR_LEN(p), 0)) {
			RETURN_FALSE;   /* plain spellings only (PHP resolves ".." lexically), and none its realpath cache holds */
		}
		break;
	}
orig:
	phasync_sc_orig[i](INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

#define PHASYNC_SC_FN(i) static ZEND_NAMED_FUNCTION(phasync_sc_fn##i) { phasync_sc_call(INTERNAL_FUNCTION_PARAM_PASSTHRU, i); }
PHASYNC_SC_FN(0) PHASYNC_SC_FN(1) PHASYNC_SC_FN(2) PHASYNC_SC_FN(3) PHASYNC_SC_FN(4) PHASYNC_SC_FN(5) PHASYNC_SC_FN(6)
PHASYNC_SC_FN(7) PHASYNC_SC_FN(8) PHASYNC_SC_FN(9) PHASYNC_SC_FN(10) PHASYNC_SC_FN(11) PHASYNC_SC_FN(12) PHASYNC_SC_FN(13)
PHASYNC_SC_FN(14) PHASYNC_SC_FN(15) PHASYNC_SC_FN(16) PHASYNC_SC_FN(17) PHASYNC_SC_FN(18) PHASYNC_SC_FN(19)
static const zif_handler phasync_sc_mine[PHASYNC_SC_NFNS] = { phasync_sc_fn0, phasync_sc_fn1, phasync_sc_fn2, phasync_sc_fn3,
	phasync_sc_fn4, phasync_sc_fn5, phasync_sc_fn6, phasync_sc_fn7, phasync_sc_fn8, phasync_sc_fn9, phasync_sc_fn10, phasync_sc_fn11,
	phasync_sc_fn12, phasync_sc_fn13, phasync_sc_fn14, phasync_sc_fn15, phasync_sc_fn16, phasync_sc_fn17, phasync_sc_fn18, phasync_sc_fn19 };

static void phasync_sc_rec_dtor(zval *zv)
{
	pefree(Z_PTR_P(zv), 1);
}

static void phasync_sc_str_dtor(zval *zv)
{
	zend_string_release(Z_TYPE_P(zv) == IS_PTR ? (zend_string *) Z_PTR_P(zv) : Z_STR_P(zv));
}

/* The child of a fork() starts empty: the fd, its owner and the watches are the parent's. The
 * parent's records are kept aside: one found unchanged by the child's lstat() keeps its serial. */
static void phasync_sc_fork_child(void)
{
	if (phasync_sc_on) {
		HashTable t = phasync_sc_inherited;
		phasync_sc_inherited = phasync_sc_recs;
		phasync_sc_recs = t;
		phasync_sc_flush();
	}
}

/* MINIT: take over the functions; phasync.stat_cache=0 (the default) leaves them alone */
static void phasync_sc_startup(void)
{
	struct sigaction sa, old;

	if (!INI_BOOL("phasync.stat_cache") || sigaction(SIGIO, NULL, &old) != 0
	 || (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN)) {
		return;   /* SIGIO is someone else's */
	}
	for (size_t i = 0; i < PHASYNC_SC_NFNS; i++) {
		if (!zend_hash_str_find_ptr(CG(function_table), phasync_sc_fns[i].name, strlen(phasync_sc_fns[i].name))) {
			return;
		}
	}
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = phasync_sc_sigio;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGIO, &sa, NULL);
	phasync_sc_cap = INI_INT("phasync.stat_cache_watches");
	phasync_sc_untrusted = INI_BOOL("phasync.test_stat_cache_untrusted");
#ifdef RUNNING_ON_VALGRIND
	phasync_sc_valgrind = RUNNING_ON_VALGRIND;
#endif
	zend_hash_init(&phasync_sc_dirs, 256, NULL, NULL, 1);
	zend_hash_init(&phasync_sc_wds, 256, NULL, phasync_sc_str_dtor, 1);
	zend_hash_init(&phasync_sc_recs, 1024, NULL, phasync_sc_rec_dtor, 1);
	zend_hash_init(&phasync_sc_inherited, 8, NULL, phasync_sc_rec_dtor, 1);
	zend_hash_init(&phasync_sc_alias, 1024, NULL, phasync_sc_str_dtor, 1);
	for (size_t i = 0; i < PHASYNC_SC_NFNS; i++) {
		zend_internal_function *f = zend_hash_str_find_ptr(CG(function_table), phasync_sc_fns[i].name, strlen(phasync_sc_fns[i].name));
		phasync_sc_orig[i] = f->handler;
		f->handler = phasync_sc_mine[i];
	}
	pthread_atfork(NULL, NULL, phasync_sc_fork_child);
	phasync_sc_on = true;
}

static void phasync_sc_shutdown(void)
{
	if (!phasync_sc_orig[0]) {
		return;
	}
	for (size_t i = 0; i < PHASYNC_SC_NFNS; i++) {
		((zend_internal_function *) zend_hash_str_find_ptr(CG(function_table), phasync_sc_fns[i].name, strlen(phasync_sc_fns[i].name)))->handler = phasync_sc_orig[i];
	}
	phasync_sc_flush();
	zend_hash_destroy(&phasync_sc_dirs);
	zend_hash_destroy(&phasync_sc_wds);
	zend_hash_destroy(&phasync_sc_recs);
	zend_hash_destroy(&phasync_sc_inherited);
	zend_hash_destroy(&phasync_sc_alias);
	signal(SIGIO, SIG_IGN);   /* one may still be on its way */
	phasync_sc_on = false;
}
#else
static bool phasync_sc_on;
static uint64_t phasync_sc_hits, phasync_sc_misses, phasync_sc_fallbacks, phasync_sc_flushes, phasync_sc_events;
static int phasync_sc_stat(const char *path, zend_stat_t *st, uint64_t *serial) { *serial = 0; return stat(path, st); }
static void phasync_sc_startup(void) {}
static void phasync_sc_shutdown(void) {}
#endif

ZEND_FUNCTION(phasync_ext_stat_cache_stats)
{
	ZEND_PARSE_PARAMETERS_NONE();

	array_init(return_value);
	add_assoc_bool(return_value, "enabled", phasync_sc_on);
	add_assoc_long(return_value, "hits", (zend_long) phasync_sc_hits);
	add_assoc_long(return_value, "misses", (zend_long) phasync_sc_misses);
	add_assoc_long(return_value, "fallbacks", (zend_long) phasync_sc_fallbacks);
	add_assoc_long(return_value, "flushes", (zend_long) phasync_sc_flushes);
	add_assoc_long(return_value, "events", (zend_long) phasync_sc_events);
#ifdef __linux__
	add_assoc_long(return_value, "watches", phasync_sc_on ? (zend_long) zend_hash_num_elements(&phasync_sc_wds) : 0);
	add_assoc_long(return_value, "entries", phasync_sc_on ? (zend_long) zend_hash_num_elements(&phasync_sc_recs) : 0);
#else
	add_assoc_long(return_value, "watches", 0);
	add_assoc_long(return_value, "entries", 0);
#endif
}
