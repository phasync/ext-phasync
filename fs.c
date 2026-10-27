/* Included by phasync.c (single translation unit); not compiled on its own. */

/* Filesystem functions made cooperative: offloading stat()/file_get_contents()/unlink() and friends to the thread pool when a mount is slow or configured to. */

/* ---- filesystem functions --------------------------------------------------
 *
 * Metadata and namespace calls are fast on a local disk and would only get slower
 * through the pool (~30µs+ a call vs ~1µs; autoloaders make thousands), so by
 * default only paths on network/FUSE mounts are offloaded, where a call can stall
 * for a round trip or worse (phasync.fs_offload=all offloads everything, =none
 * nothing). Read-only calls stat (or read) the path on the pool, warming the
 * kernel's caches, and then run the original, which answers from them — results,
 * PHP's stat cache and warnings stay native. unlink/rmdir/mkdir/rename run on the
 * pool; on failure the original runs to fail again with the native warning. */

static bool phasync_fs_slow_type(const char *type)
{
	static const char *const slow[] = {
		"nfs", "nfs4", "cifs", "smb3", "smbfs", "9p", "ceph", "glusterfs", "lustre",
		"gpfs", "beegfs", "afs", "virtiofs", "davfs", "afpfs", "webdav", "macfuse", "osxfuse", NULL
	};
	const char *const *s;

	if (strcmp(type, "fuse") == 0 || strcmp(type, "fuseblk") == 0 || strncmp(type, "fuse.", 5) == 0
	 || strcmp(type, "fusefs") == 0) {
		return true;                     /* fuse.sshfs, fuse.s3fs, ...: userland */
	}
	for (s = slow; *s; s++) {
		if (strcmp(type, *s) == 0) {
			return true;
		}
	}
	return false;
}

/* Is this type listed in phasync.fs_offload_types (comma-separated)? */
static bool phasync_fs_listed_type(const char *type)
{
	const char *l = PHASYNC_G(fs_offload_types);
	size_t tl = strlen(type);

	while (l && *l) {
		l += strspn(l, " ,");
		size_t n = strcspn(l, " ,");
		if (n == tl && n && strncmp(l, type, n) == 0) {
			return true;
		}
		l += n;
	}
	return false;
}

static void phasync_mounts_free(void)
{
	for (int i = 0; i < PHASYNC_G(nmounts); i++) {
		free(PHASYNC_G(mounts)[i].path);
	}
	free(PHASYNC_G(mounts));
	PHASYNC_G(mounts) = NULL;
	PHASYNC_G(nmounts) = 0;
	PHASYNC_G(any_slow_mount) = false;
	PHASYNC_G(any_pool_mount) = false;
	free(PHASYNC_G(nowait_off));         /* a device number may now be another filesystem */
	PHASYNC_G(nowait_off) = NULL;
	PHASYNC_G(nnowait_off) = 0;
	PHASYNC_G(fs_last) = NULL;
}

static bool phasync_mount_add(const char *mnt, dev_t dev, const char *type)
{
	if (PHASYNC_G(nmounts) % 64 == 0) {
		phasync_mount *nm = realloc(PHASYNC_G(mounts), (PHASYNC_G(nmounts) + 64) * sizeof(*nm));
		if (nm == NULL) {
			return false;
		}
		PHASYNC_G(mounts) = nm;
	}
	phasync_mount *m = &PHASYNC_G(mounts)[PHASYNC_G(nmounts)++];
	m->path = strdup(mnt);
	m->len = strlen(mnt);
	m->dev = dev;
	m->slow = phasync_fs_slow_type(type);
	m->pool = m->slow || phasync_fs_listed_type(type);
	PHASYNC_G(any_slow_mount) |= m->slow;
	PHASYNC_G(any_pool_mount) |= m->pool;
	return true;
}

/* Keep only the mounts a lookup needs. */
static void phasync_mounts_prune(void)
{
	/* A lookup can only come out slow on a slow mount or one at or under it (that
	 * shadows it): keep just those, in order, so a lookup scans a handful; and the
	 * pool mounts, matched by device. */
	int kept = 0;
	for (int i = 0; i < PHASYNC_G(nmounts); i++) {
		phasync_mount *m = &PHASYNC_G(mounts)[i];
		bool keep = m->pool;
		for (int j = 0; j < PHASYNC_G(nmounts) && !keep; j++) {
			phasync_mount *s = &PHASYNC_G(mounts)[j];
			keep = s->slow && strncmp(m->path, s->path, s->len) == 0
				&& (m->path[s->len] == '/' || m->path[s->len] == '\0' || s->len == 1);
		}
		if (keep) {
			PHASYNC_G(mounts)[kept++] = *m;
		} else {
			free(m->path);
		}
	}
	PHASYNC_G(nmounts) = kept;
}

#ifdef __linux__
/* (Re)load the mount table when it changed: /proc/self/mountinfo polls POLLPRI
 * after a mount or unmount. Polled at most once a second (the clock is read in
 * the vDSO, no syscall), so a filesystem call costs no extra syscall and a new
 * mount is noticed within a second. Lines look like
 *   36 35 98:0 /mnt1 /mnt/parent rw,noatime master:1 - ext3 /dev/root rw */
static void phasync_mounts_refresh(void)
{
	int fd = PHASYNC_G(mountinfo_fd);
	char *buf = NULL, *line, *next;
	size_t size = 0, len = 0;
	ssize_t n;

	if (fd == -2) {
		return;                          /* no /proc: nothing counts as slow */
	}
	if (fd == -1) {
		if ((fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC)) < 0) {
			PHASYNC_G(mountinfo_fd) = -2;
			return;
		}
		PHASYNC_G(mountinfo_fd) = fd;
	} else {
		struct pollfd pfd = { fd, POLLPRI, 0 };
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
		if (now.tv_sec == PHASYNC_G(mounts_checked)) {
			return;                      /* polled this second already */
		}
		PHASYNC_G(mounts_checked) = now.tv_sec;
		if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & (POLLPRI | POLLERR))) {
			return;                      /* unchanged */
		}
	}
	lseek(fd, 0, SEEK_SET);
	do {
		if (len + 4096 > size) {
			char *nb = realloc(buf, size = size ? size * 2 : 16384);
			if (nb == NULL) {
				free(buf);
				return;
			}
			buf = nb;
		}
		n = read(fd, buf + len, size - len - 1);
		len += n > 0 ? (size_t) n : 0;
	} while (n > 0);
	buf[len] = '\0';

	phasync_mounts_free();
	for (line = buf; *line; line = next) {
		char *mnt = line, *end, *type, *o, *i;
		unsigned int major = 0, minor = 0;
		int field;

		next = strchr(line, '\n');
		next = next ? (*next = '\0', next + 1) : line + strlen(line);
		for (field = 0; field < 4 && mnt; field++) {
			mnt = strchr(mnt, ' ');
			mnt = mnt ? mnt + 1 : NULL;
		}
		if (!mnt || !(end = strchr(mnt, ' ')) || !(type = strstr(end, " - "))
		 || sscanf(line, "%*s %*s %u:%u", &major, &minor) != 2) {
			continue;
		}
		*end = '\0';
		type += 3;
		if ((o = strchr(type, ' '))) {
			*o = '\0';
		}
		for (i = o = mnt; *i; o++) {     /* unescape \ooo (spaces etc.) */
			if (i[0] == '\\' && i[1] >= '0' && i[1] <= '3' && i[2] >= '0' && i[2] <= '7' && i[3] >= '0' && i[3] <= '7') {
				*o = (char) (((i[1] - '0') << 6) | ((i[2] - '0') << 3) | (i[3] - '0'));
				i += 4;
			} else {
				*o = *i++;
			}
		}
		*o = '\0';
		if (!phasync_mount_add(mnt, makedev(major, minor), type)) {
			break;
		}
	}
	free(buf);

	phasync_mounts_prune();
}
#else
/* (Re)load the mount table when it changed, checked at most once a second: one
 * getmntinfo() call, compared by a hash of the mount points, types and ids. */
static void phasync_mounts_refresh(void)
{
	struct statfs *fs;
	struct timespec now;
	uint64_t sig = 14695981039346656037ULL;
	int n;

	clock_gettime(PHASYNC_CLOCK_COARSE, &now);
	if (PHASYNC_G(mounts_sig) && now.tv_sec == PHASYNC_G(mounts_checked)) {
		return;                          /* checked this second already */
	}
	PHASYNC_G(mounts_checked) = now.tv_sec;
	if ((n = getmntinfo(&fs, MNT_NOWAIT)) <= 0) {
		return;
	}
	for (int i = 0; i < n; i++) {
		const unsigned char *b = (const unsigned char *) &fs[i].f_fsid;
		for (size_t k = 0; k < sizeof(fs[i].f_fsid); k++) {
			sig = (sig ^ b[k]) * 1099511628211ULL;
		}
		for (const char *c = fs[i].f_mntonname; *c; c++) {
			sig = (sig ^ (unsigned char) *c) * 1099511628211ULL;
		}
		for (const char *c = fs[i].f_fstypename; *c; c++) {
			sig = (sig ^ (unsigned char) *c) * 1099511628211ULL;
		}
	}
	sig |= 1;                            /* never 0: 0 means not loaded */
	if (sig == PHASYNC_G(mounts_sig)) {
		return;                          /* unchanged */
	}
	PHASYNC_G(mounts_sig) = sig;
	phasync_mounts_free();
	for (int i = 0; i < n; i++) {
		if (!phasync_mount_add(fs[i].f_mntonname, (dev_t) fs[i].f_fsid.val[0], fs[i].f_fstypename)) {
			break;
		}
	}
	phasync_mounts_prune();
}
#endif

/* Is this absolute path offloaded? Under the network policy: when the mount it
 * lies on (lexically, the longest matching mount point; the later of stacked
 * ones) is a network/FUSE filesystem. */
static bool phasync_fs_offloaded(const char *path)
{
	int best = -1;
	size_t bestlen = 0;

	if (PHASYNC_G(fs_offload) != PHASYNC_FS_OFFLOAD_NETWORK) {
		return PHASYNC_G(fs_offload) == PHASYNC_FS_OFFLOAD_ALL;
	}
	phasync_mounts_refresh();
	if (!PHASYNC_G(any_slow_mount)) {
		return false;
	}
	for (int i = 0; i < PHASYNC_G(nmounts); i++) {
		phasync_mount *m = &PHASYNC_G(mounts)[i];
		if (m->len >= bestlen && strncmp(path, m->path, m->len) == 0
		 && (path[m->len] == '/' || path[m->len] == '\0' || m->len == 1)) {
			best = i;
			bestlen = m->len;
		}
	}
	return best >= 0 && PHASYNC_G(mounts)[best].slow;
}

/* How do reads of this descriptor run in a coroutine (PHASYNC_FS_*)? A regular
 * file on a network/FUSE mount, or on a filesystem type listed in
 * phasync.fs_offload_types, reads on the pool (=all: every regular file). Any
 * other is read with RWF_NOWAIT, inline from the page cache and on the pool only
 * when it would wait for the disk, unless its filesystem refused RWF_NOWAIT
 * (ZFS, for one): then inline, as natively. While no such mount is known, this
 * costs no syscall; the reader tells a regular file only when a read would
 * block. */
static int phasync_fs_fd_mode(php_socket_t fd)
{
	struct stat st;

	if (PHASYNC_G(fs_offload) == PHASYNC_FS_OFFLOAD_NONE) {
		return PHASYNC_FS_INLINE;
	}
	if (PHASYNC_G(fs_offload) == PHASYNC_FS_OFFLOAD_NETWORK) {
		phasync_mounts_refresh();
		if (!PHASYNC_G(any_pool_mount) && PHASYNC_G(nnowait_off) == 0) {
			return PHASYNC_HAVE_NOWAIT ? PHASYNC_FS_NOWAIT : PHASYNC_FS_INLINE;
		}
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		return PHASYNC_FS_INLINE;
	}
	if (PHASYNC_G(fs_offload) == PHASYNC_FS_OFFLOAD_ALL) {
		return PHASYNC_FS_POOL;
	}
	for (int i = 0; i < PHASYNC_G(nmounts); i++) {
		if (PHASYNC_G(mounts)[i].pool && PHASYNC_G(mounts)[i].dev == st.st_dev) {
			return PHASYNC_FS_POOL;
		}
	}
	for (int i = 0; i < PHASYNC_G(nnowait_off); i++) {
		if (PHASYNC_G(nowait_off)[i] == st.st_dev) {
			return PHASYNC_FS_INLINE;
		}
	}
	return PHASYNC_HAVE_NOWAIT ? PHASYNC_FS_NOWAIT : PHASYNC_FS_INLINE;
}

static bool phasync_fs_fd_offloaded(php_socket_t fd)
{
	return phasync_fs_fd_mode(fd) == PHASYNC_FS_POOL;
}

/* Copy a plain local path argument into out (PATH_MAX), made absolute. Returns
 * 1 if it is offloaded, 0 if not, -1 if it isn't a plain local path (a stream
 * wrapper, an embedded NUL, too long: then the original handles it). */
static int phasync_fs_path(zval *z, char *out)
{
	size_t len, cwdlen = 0;

	if (Z_TYPE_P(z) != IS_STRING || (len = Z_STRLEN_P(z)) == 0
	 || strlen(Z_STRVAL_P(z)) != len || strstr(Z_STRVAL_P(z), "://") != NULL) {
		return -1;
	}
	if (Z_STRVAL_P(z)[0] != '/') {
		if (VCWD_GETCWD(out, PATH_MAX) == NULL) {
			return -1;
		}
		cwdlen = strlen(out);
		out[cwdlen++] = '/';
	}
	if (cwdlen + len >= PATH_MAX) {
		return -1;
	}
	memcpy(out + cwdlen, Z_STRVAL_P(z), len + 1);
	return phasync_fs_offloaded(out) ? 1 : 0;
}

static ZEND_NAMED_FUNCTION(phasync_fs_override)
{
	/* Keyed by the name, not the function: a closure of it (is_file(...),
	 * Closure::fromCallable()) calls a copy that shares the name (#9). */
	zval *idx = zend_hash_index_find(&PHASYNC_G(fs_hooks), (zend_ulong) (uintptr_t) EX(func)->common.function_name);
	zif_handler orig = PHASYNC_G(orig_fs)[Z_LVAL_P(idx)];
	uint32_t argc = ZEND_NUM_ARGS();
	phasync_task t;
	char *p;

	memset(&t, 0, sizeof(t));
	t.fsop = phasync_fs_funcs[Z_LVAL_P(idx)].op;
	if (!phasync_reading() || EG(active_fiber) == NULL || argc < 1) {
		orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}
	switch (t.fsop) {
		case PHASYNC_FS_WARM:
		case PHASYNC_FS_WARM_DIR:
			if (phasync_fs_path(ZEND_CALL_ARG(execute_data, 1), t.path) != 1) {
				break;
			}
			t.type = PHASYNC_OP_FS;
			phasync_pool_run(&t);
			break;
		case PHASYNC_FS_WARM_GLOB:
			if (phasync_fs_path(ZEND_CALL_ARG(execute_data, 1), t.path) != 1) {
				break;
			}
			t.path[strcspn(t.path, "*?[{")] = '\0';   /* the directory before the first wildcard */
			*(strrchr(t.path, '/') + (strrchr(t.path, '/') == t.path)) = '\0';
			t.type = PHASYNC_OP_FS;
			phasync_pool_run(&t);
			break;
		case PHASYNC_FS_UNLINK:
		case PHASYNC_FS_RMDIR:
		case PHASYNC_FS_MKDIR:
		case PHASYNC_FS_RENAME: {
			/* No stream context, plain argument types, and no open_basedir refusal
			 * (the original reports that); anything else is left to the original. */
			uint32_t nargs = t.fsop == PHASYNC_FS_RENAME ? 2 : t.fsop == PHASYNC_FS_MKDIR ? 3 : 1;
			zval *ctx = argc > nargs ? ZEND_CALL_ARG(execute_data, nargs + 1) : NULL;
			int slow;

			if ((ctx && Z_TYPE_P(ctx) != IS_NULL)
			 || (slow = phasync_fs_path(ZEND_CALL_ARG(execute_data, 1), t.path)) < 0
			 || (PG(open_basedir) && *PG(open_basedir) && php_check_open_basedir_ex(t.path, 0))) {
				break;
			}
			if (t.fsop == PHASYNC_FS_RENAME) {
				int slow2;
				if (argc < 2 || (slow2 = phasync_fs_path(ZEND_CALL_ARG(execute_data, 2), t.path2)) < 0
				 || (PG(open_basedir) && *PG(open_basedir) && php_check_open_basedir_ex(t.path2, 0))) {
					break;
				}
				slow |= slow2;
			}
			if (t.fsop == PHASYNC_FS_MKDIR) {
				zval *mode = argc >= 2 ? ZEND_CALL_ARG(execute_data, 2) : NULL;
				zval *rec  = argc >= 3 ? ZEND_CALL_ARG(execute_data, 3) : NULL;
				if ((mode && Z_TYPE_P(mode) != IS_LONG) || (rec && Z_TYPE_P(rec) != IS_TRUE && Z_TYPE_P(rec) != IS_FALSE)) {
					break;
				}
				t.omode = mode ? (int) Z_LVAL_P(mode) : 0777;
				if (rec && Z_TYPE_P(rec) == IS_TRUE) {
					t.fsop = PHASYNC_FS_MKDIR_P;
					for (p = t.path + strlen(t.path) - 1; p > t.path && *p == '/'; p--) {
						*p = '\0';               /* mkdir -p a/b/ creates a/b */
					}
				}
			}
			if (!slow) {
				break;
			}
			t.type = PHASYNC_OP_FS;
			phasync_pool_run(&t);
			if (EG(exception)) {
				return;
			}
			if (t.result == 0) {
				if (t.fsop != PHASYNC_FS_MKDIR && t.fsop != PHASYNC_FS_MKDIR_P) {
					php_clear_stat_cache(1, NULL, 0);   /* as the native op does */
				}
				RETURN_TRUE;
			}
			break;                       /* failed: the original reports it natively */
		}
		default:
			break;
	}
	if (EG(exception)) {
		return;
	}
	orig(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}


/* ---- SQLite: the "phasync" VFS --------------------------------------------
 *
 * A VFS wrapping SQLite's "unix" one is registered as the default when libsqlite3
 * is in the process (sqlite3 and pdo_sqlite link it). Every file keeps the unix
 * VFS's own methods, with three exceptions in a coroutine: xSync runs on the
 * pool while the coroutine parks, xRead and xWrite too for a file on a mount
 * phasync_fs_offloaded() names, and xSleep (the busy timeout's retries) is a
 * cooperative sleep. Locking, shared memory, mmap, open and close stay the unix
 * VFS's, on the same file object. A call on the pool is waited for to its end,
 * even when cancelled: it works on SQLite's own file and buffer. Connections
 * opened with another VFS keep theirs. */
#if __has_include(<sqlite3.h>)
# include <sqlite3.h>

typedef struct {
	sqlite3_io_methods m;                /* first: a file's pMethods points here */
	const sqlite3_io_methods *orig;
} phasync_vfs_methods;

typedef struct {
	const phasync_vfs_methods *t;
	sqlite3_file *f;
	void *buf;
	int amt, flags, rc, op;
	sqlite3_int64 off;
} phasync_vfs_op;

static sqlite3_vfs phasync_vfs, *phasync_vfs_unix;
static int (*phasync_vfs_unregister)(sqlite3_vfs *);
static phasync_vfs_methods phasync_vfs_tables[8][2];    /* per unix method set: [local, offloaded] */
static int phasync_vfs_ntables;
static bool phasync_vfs_threads;                        /* libsqlite3 is thread-safe */

static void phasync_vfs_exec(void *arg)
{
	phasync_vfs_op *o = arg;

	o->rc = o->op == 0 ? o->t->orig->xSync(o->f, o->flags)
	      : o->op == 1 ? o->t->orig->xRead(o->f, o->buf, o->amt, o->off)
	      : o->t->orig->xWrite(o->f, o->buf, o->amt, o->off);
}

/* Run the original method on the pool in a coroutine, inline elsewhere. */
static int phasync_vfs_run(phasync_vfs_op *o)
{
	phasync_task t;

	o->t = (const phasync_vfs_methods *) o->f->pMethods;
	if (!phasync_vfs_threads || !phasync_reading() || EG(active_fiber) == NULL) {
		phasync_vfs_exec(o);
		return o->rc;
	}
	memset(&t, 0, sizeof(t));
	t.type = PHASYNC_OP_CALL;
	t.call = phasync_vfs_exec;
	t.arg = o;
	phasync_pool_run(&t);
	return o->rc;
}

static int phasync_vfs_sync(sqlite3_file *f, int flags)
{
	phasync_vfs_op o = { .f = f, .flags = flags, .op = 0 };
	return phasync_vfs_run(&o);
}

static int phasync_vfs_read(sqlite3_file *f, void *buf, int amt, sqlite3_int64 off)
{
	phasync_vfs_op o = { .f = f, .buf = buf, .amt = amt, .off = off, .op = 1 };
	return phasync_vfs_run(&o);
}

static int phasync_vfs_write(sqlite3_file *f, const void *buf, int amt, sqlite3_int64 off)
{
	phasync_vfs_op o = { .f = f, .buf = (void *) buf, .amt = amt, .off = off, .op = 2 };
	return phasync_vfs_run(&o);
}

static int phasync_vfs_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *f, int flags, int *out)
{
	int rc = phasync_vfs_unix->xOpen(vfs, name, f, flags, out), i;
	const sqlite3_io_methods *m = f->pMethods;

	if (rc != SQLITE_OK || m == NULL) {
		return rc;
	}
	for (i = 0; i < phasync_vfs_ntables && phasync_vfs_tables[i][0].orig != m; i++);
	if (i == phasync_vfs_ntables) {
		if (i == sizeof(phasync_vfs_tables) / sizeof(phasync_vfs_tables[0])) {
			return rc;
		}
		for (int r = 0; r < 2; r++) {
			phasync_vfs_tables[i][r].m = *m;
			phasync_vfs_tables[i][r].orig = m;
			phasync_vfs_tables[i][r].m.xSync = phasync_vfs_sync;
		}
		phasync_vfs_tables[i][1].m.xRead = phasync_vfs_read;
		phasync_vfs_tables[i][1].m.xWrite = phasync_vfs_write;
		phasync_vfs_ntables++;
	}
	f->pMethods = &phasync_vfs_tables[i][name && phasync_fs_offloaded(name)].m;
	return rc;
}

static int phasync_vfs_sleep(sqlite3_vfs *vfs, int us)
{
	if (phasync_scope_top() == NULL || EG(active_fiber) == NULL || phasync_from_c()) {
		return phasync_vfs_unix->xSleep(vfs, us);
	}
	if (!EG(exception)) {                /* pending (a cancellation): the retries run out at once */
		phasync_call_sleep(us);
	}
	return us;
}

static void phasync_vfs_register(void)
{
	sqlite3_vfs *(*find)(const char *) = (sqlite3_vfs *(*)(const char *)) dlsym(RTLD_DEFAULT, "sqlite3_vfs_find");
	int (*reg)(sqlite3_vfs *, int) = (int (*)(sqlite3_vfs *, int)) dlsym(RTLD_DEFAULT, "sqlite3_vfs_register");
	int (*threadsafe)(void) = (int (*)(void)) dlsym(RTLD_DEFAULT, "sqlite3_threadsafe");

	phasync_vfs_unregister = (int (*)(sqlite3_vfs *)) dlsym(RTLD_DEFAULT, "sqlite3_vfs_unregister");
	if (!find || !reg || !threadsafe || !phasync_vfs_unregister || (phasync_vfs_unix = find("unix")) == NULL) {
		return;
	}
	phasync_vfs = *phasync_vfs_unix;
	phasync_vfs.pNext = NULL;
	phasync_vfs.zName = "phasync";
	phasync_vfs.xOpen = phasync_vfs_open;
	phasync_vfs.xSleep = phasync_vfs_sleep;
	phasync_vfs_threads = threadsafe() != 0;
	if (reg(&phasync_vfs, 1) != SQLITE_OK) {
		phasync_vfs_unix = NULL;
	}
}

/* Files still open keep the method tables, which live as long as this module. */
static void phasync_vfs_shutdown(void)
{
	if (phasync_vfs_unix) {
		phasync_vfs_unregister(&phasync_vfs);
		phasync_vfs_unix = NULL;
	}
}
#else
static void phasync_vfs_register(void) {}
static void phasync_vfs_shutdown(void) {}
#endif
