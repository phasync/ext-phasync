# Stat cache

`phasync.stat_cache=1` (`PHP_INI_SYSTEM`, default `0`, Linux only) answers the stat family from
remembered results that inotify keeps valid. Source: `statcache.c`. Counters:
`phasync\ext\stat_cache_stats()`.

## Why

A profile of WordPress (block theme), WordPress with 25 plugins, Laravel and MediaWiki under Swerve
found `file_exists()`, `is_file()`, `is_readable()`, `filemtime()` and friends at 84–100 % repeat
across requests, each one `stat()` or `access()` syscall (1.3–1.7 µs). In plugin-heavy WordPress
that is ~1,900 syscalls and ~10 ms per request; half of the `access()` calls are Composer's
autoloader probing `vendor/composer/../../src/...` paths, and two thirds of those that fail do so
because a parent directory is missing. A cache that still makes one syscall per hit saves little;
this one makes none.

## Per-worker design (built)

**What is cached.** For an absolute path, as PHP got it, through the plain-files wrapper (checked
with `php_stream_locate_url_wrapper()`, so a user wrapper registered as `file` is respected), with
no NUL byte and allowed by `open_basedir` (checked quietly first; when it says no, PHP's own call
raises the warning): one `lstat()` result, plus `access(R_OK/W_OK/X_OK)` results asked for later.
Relative paths, a trailing `/`, other wrappers: PHP's own call, uncached.

**Watches.** One inotify instance per process, created on first use. Every directory from `/` down
to the path's parent gets a watch (`IN_ATTRIB|IN_MODIFY|IN_CLOSE_WRITE|IN_CREATE|IN_DELETE|
IN_MOVED_FROM|IN_MOVED_TO|IN_DELETE_SELF|IN_MOVE_SELF`) with `IN_DONT_FOLLOW|IN_ONLYDIR`. Ancestors
are shared by every path below them: plugin-heavy WordPress needs ~340 watches. Watching the whole
chain answers three questions at once:

- *An ancestor renamed, deleted, replaced or chmod'ed*: its own watch reports `IN_MOVE_SELF`,
  `IN_DELETE_SELF` or `IN_ATTRIB`, and its parent's watch reports the name. Either drops
  everything (rare: deploys).
- *Symlinks*: `IN_DONT_FOLLOW|IN_ONLYDIR` fails on a symlink, so a watched chain holds none, and a
  path through a symlinked directory is not cached. A symlink as the last component is cached for
  `lstat()`/`is_link()`/`filetype()` only; following calls go to PHP.
- *`..`*: with every component a real directory, `a/b/../c` is `a/c`. Paths are resolved
  lexically to a canonical one; the spelling is remembered as an alias of it.

A path whose parent is missing (or no directory) is a cached failure tied to the first missing
component, a child of a watched directory: creating it there drops it. A cached directory is
itself watched, as its own stat changes with its entries.

**Events.** An event on watched directory `D` about name `N` drops the result for `D/N` (and `D`'s
own on create/delete/move). If `D/N` is a directory we watch (or chose not to cache through), or
the event is about `D` itself, or the queue overflowed, everything is dropped: the fd is closed
(removing all watches at once) and the tables cleared.

**Delivery.** The fd is `O_ASYNC`, owned by the main thread (`F_SETOWN_EX`/`F_OWNER_TID`); the
kernel raises `SIGIO` as it queues an event, and the handler sets a flag. A lookup reads the flag;
when set, it drains the fd (non-blocking reads) before answering. So:

- a hit costs no syscall;
- another process's change is seen once the kernel has queued the event and signalled us
  (delivery on the next return to user space; an IPI if we are running elsewhere: microseconds);
- this process's own writes, through any PHP function or stream, are seen at once: the signal is
  pending when the writing syscall returns. Hooking `fopen()`/`file_put_contents()`/`unlink()`/...
  was the alternative; it would miss `fwrite()` on an open stream, `SplFileObject`, `ZipArchive`,
  extensions. Not needed.

The fd is not registered with the poller, and `run_next()` does not drain it: the flag already says
when there is something to read, and events wait in the kernel queue meanwhile (overflow drops
everything). Under valgrind (which delivers signals late) every lookup drains.

Options measured, per `file_exists()` hit (PHP 8.5, JIT, pinned):

| check before a hit | ns / call |
|---|---|
| none (cache off): `access()` | 1,770 |
| non-blocking `read()` of the inotify fd every hit | 950 |
| SIGIO flag (built) | 250 |

`is_file()`/`filemtime()` pairs: 4,500 off, 2,000 with a read per hit, 590 with the flag.

**PHP semantics.** `file_exists()`, `is_readable()`, `is_writable()`, `is_executable()` are
`access()` calls in PHP, never cached by it; we answer them directly (`F_OK` from the `lstat()`
result; `R/W/X` from `access()`, asked once). The `stat()`-based functions first let PHP's own
one-entry stat cache (`BG(CurrentStatFile)`) answer when it holds the path, stale as it may be,
exactly as without us; otherwise our result is put there and PHP's own function runs, so the result
and its formatting are PHP's. A failure where PHP warns (`filemtime()` of a missing file) is left
to PHP. `realpath()` is answered only when it fails, for plain spellings PHP's realpath cache does
not hold (PHP resolves `..` lexically before looking).

`clearstatcache()` runs PHP's, then drains pending events; with `$filename`, that path's result
(under any spelling) is dropped. Without one it does **not** drop the cache: the results are as
current as the event queue, and WordPress and Laravel call `clearstatcache()` once per request,
which would cut the hit rate to within-request repeats.

**Fallbacks** (PHP's own call): `statfs()` of each new directory; only ext2/3/4, xfs, btrfs, zfs,
tmpfs, ramfs, f2fs, bcachefs, overlayfs, squashfs and erofs are trusted. NFS, CIFS/SMB, FUSE
(sshfs, virtiofs, Docker Desktop shares), 9p, Ceph, vboxsf and anything unknown are not: other
machines' changes raise no event there. overlayfs is trusted for changes made through it (checked:
a copy-up of a lower file raises events); changing a lower layer under a mounted overlay is
undefined behaviour per the kernel docs. Also: `inotify_init1()` failing (`max_user_instances`,
128 per user here: every worker takes one) leaves the cache off for that process; an
`inotify_add_watch()` failure (`max_user_watches`, `EACCES`) leaves that directory uncached; a
directory bind-mounted at two paths (one wd) is not cached at its second path; non-Linux builds
compile it out. `phasync.test_stat_cache_untrusted=1` makes every filesystem untrusted (tests).

**Limits.** At most `phasync.stat_cache_watches` (default 4096) watches and 65,536 results +
spellings; reaching either drops everything and starts over (a working set larger than the cap
thrashes: then raise it). A `fork()`ed child starts empty (it must not read its parent's fd). A
SIGIO handler installed before MINIT turns the cache off; one installed later (`pcntl_signal(SIGIO)`)
or SIGIO blocked by the application would leave results stale. Interrupted `nanosleep()`/`poll()`
return early on a signal even with `SA_RESTART`: native `usleep()` outside a phasync scope can end
early when a watched directory changes. `setuid()` after results were cached is not noticed.
`SplFileInfo` methods call `php_stat()` directly and are not cached (Laravel's config loading and
session GC go through Symfony Finder).

**Memo.** The memo's `file_get_contents()` key and `scandir()`/`glob()` listing key take the
cached `stat()` plus the result's serial, new for each lstat: an entry added within one timestamp
tick (same mtime, same size) changes the key too. A forked child's serials differ from its
parent's, so inherited memo entries miss once in the child.

## Measurements

One Swerve worker (`-S docroot`, `--workers=1`, pinned to CPU 16, curl on 20–23), 300 sequential
requests per run over each app's URL list, cache off (`sc0`) and on (`sc1`) in alternating order,
one run at a time under the shared bench lock, all 200 (a run with any other status is void: a
first attempt on the wrong port got WordPress's canonical 301s, with the cache off as on).
Load average 2–4. CPU ms per request of the worker's process group:

| app | off | on | saved |
|---|---|---|---|
| WordPress, 25 plugins (3 runs each) | 201.1 (199.4–211.2) | 195.6 (194.3–196.5) | ~5.5 ms, 2.7 % |
| MediaWiki (2 runs each) | 91.0, 92.4 | 90.8, 90.8 | ~0.9 ms, 1 % |
| WordPress, block theme (3 runs each) | 69.2, 69.9, 69.0 | 68.7, 68.4, 68.8 | ~0.5 ms, 0.7 % |
| Laravel (1 run each, load 5–14) | 9.15 | 8.83 | not conclusive |

Stat-family syscalls per request (strace of the worker, 30 requests; 100 for Laravel):

| app | `access` off → on | `stat` off → on | `lstat` off → on |
|---|---|---|---|
| WordPress, 25 plugins | 1717 → 14 | 121 → 9 | 235 → 194 |
| MediaWiki | 24 → 1 | 186 → 11 | 0 → 0 |
| WordPress, block theme | 112 → 2 | 9 → 2 | 8 → 1 |
| Laravel | 21 → 1 | 54 → 58 | 43 → 61 |

What remains: in WordPress, `lstat()`s from `glob()` itself (plugin `lang/` directories; the memo
answers those listings when `phasync.memo_memory` is set) and realpath-cache misses; in Laravel,
`SplFileInfo`/Finder stats (not overridden) and the session garbage collector's `filemtime()` on
thousands of one-off session files, which are misses either way. Response bodies are the same
with the cache on and off (compared over the WordPress URL list). The saving is about half of what
the profile predicted for WordPress: the profiler's per-call overhead inflated the stat family's
share.

## Next step: one cache for all workers (proposal, not built)

Every Swerve worker now runs its own instance: N workers × ~340 watches, N of the 128 instances
per user, N copies of every result. Swerve's `--watch` is no help as it stands: it polls
(`Supervisor::snapshot()`, a `RecursiveDirectoryIterator` + `stat()` scan every `WATCH_EVERY`),
it has no inotify instance.

Proposal: the ext owns it, Swerve only calls it.

- **Master, before forking workers:** `phasync\ext\stat_cache_share(int $slots = 1 << 16): void`
  creates one inotify instance and an anonymous `MAP_SHARED` region inherited by every worker:
  `uint32_t epoch` and `uint32_t gen[$slots]`.
- **Watches:** a worker adds watches to the inherited fd itself (`inotify_add_watch()` works on any
  process's copy of the instance; the wd is per instance, so all workers agree on it). No path
  table in shared memory: counters are indexed by wd.
- **Events:** whoever drains the fd applies them: name events bump
  `gen[hash(wd, name) % slots]`, create/delete/move also `gen[wd % slots]` (the directory's own
  result), directory-self and `IN_ISDIR` move/delete/attrib events and overflows bump `epoch`.
- **Hit in a worker:** result valid iff `epoch` and its `gen` slot (and, for a directory, its
  `wd` slot) still hold the values recorded at fill time: two relaxed loads, no syscall, no signal.
  A collision only causes a miss.
- **Delivery:** `F_SETOWN` to the negative process group: every worker gets SIGIO as an event is
  queued. A worker with the flag set drains the fd under a shared spinlock (seqlock-style
  `draining` counter in the region); a worker that finds the queue empty while another drains
  waits for `draining` to settle before trusting its counters. The master drains in its loop too,
  so idle workers do not let the queue overflow.
- **Cap:** past the watch cap, the drainer bumps `epoch` and removes all watches
  (`inotify_rm_watch()` of every wd it saw; or the master replaces the instance and hands the new
  fd to workers on their next `run_next()`).

Interface for Swerve: one call in the master before the first fork, under the same INI switch;
nothing else changes. Open questions: whether the process-group SIGIO also reaches Swerve's
WebSocket workers (harmless: they just set a flag), and fd inheritance across Swerve's reload
(`--watch` restart re-forks from the master: the region and fd survive).
