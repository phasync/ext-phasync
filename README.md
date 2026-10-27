# ext-phasync

[![CI](https://github.com/phasync/ext-phasync/actions/workflows/ci.yml/badge.svg)](https://github.com/phasync/ext-phasync/actions/workflows/ci.yml)

**Make the PHP you already have cooperate.** Inside a [phasync](https://github.com/phasync/phasync)
coroutine, a MySQL query through PDO, a Guzzle request or a `file_get_contents()` call waits as a coroutine
instead of blocking the process, and returns exactly what PHP would have returned. No new client
libraries, no changes to your code:

- **Under PHP-FPM**, one request overlaps its database queries, API calls and file work.
- **Under [swerve](https://github.com/phasync/swerve)**, a worker keeps thousands of connections
  busy, waiting on them with epoll: at 50,000 connections a hello-world server handled 2 to 4
  times as many requests per second as with PHP's own `stream_select()`.

`pie install phasync/ext-phasync` builds and enables it; `composer require phasync/phasync` carries
prebuilt binaries. MIT, no dependencies: [the Ennerd philosophy](PHILOSOPHY.md).

---

A PHP extension that gives [phasync](https://github.com/phasync/phasync) — and any
fiber-based event loop — transparent async I/O on **PHP 8.3+**, without patching PHP:

1. **`phasync\ext\Suspension`** — a loop opts its fibers in with `Suspension::manage()`.
   A blocking call in one suspends it and throws a `Suspension` out of the
   `start()`/`resume()` that ran it; the loop waits for its Suspensions with
   `Suspension::poll()`, on epoll and **not bounded by `FD_SETSIZE`** (the ~1024
   descriptor ceiling), and resumes each when it is ready. The blocking calls
   covered:
   - `tcp://` / `unix://` / `udp://` / `udg://` sockets (incl. `fsockopen`,
     `stream_socket_client/server`), including `tcp://` connect + its DNS lookup,
     `stream_socket_accept()`, and `stream_socket_recvfrom()`/`sendto()`
   - `ssl://` / `tls://` sockets, including the TLS handshake (in `tls://` connect
     and accept, and `stream_socket_enable_crypto()` on a blocking stream, after
     which the stream's reads and writes go through TLS)
   - `stream_select()` and `socket_select()`
   - ext/sockets: `socket_read()`, `socket_recv()`, `socket_recvfrom()`,
     `socket_write()`, `socket_send()`, `socket_sendto()`, `socket_accept()`,
     `socket_connect()` (numeric addresses and unix paths), honouring
     `SO_RCVTIMEO`/`SO_SNDTIMEO` as native timeouts
   - `curl_multi_select()`, so curl-multi loops (Guzzle's multi handler) don't block
     (probed without waiting and slept on between probes, 1ms doubling to 20ms:
     PHP doesn't expose curl's sockets), and `curl_exec()`, run on a private
     curl_multi with native results and errors (callbacks run in the coroutine)
   - php-memcached (PECL `memcached`): every `Memcached` method and the
     `memcached` session handler wait for the server through the coroutine, with
     libmemcached's own results and errors (see below)
   - PostgreSQL (`pg_*()` and PDO pgsql, over libpq): connecting, TLS and every
     query wait through the coroutine, with libpq's own results and errors
   - SQLite (SQLite3 and PDO sqlite): a busy database's retries sleep through the
     loop, and fsync runs on the thread pool (see below)
   - `sem_acquire()` and `msg_receive()` (their non-blocking forms retried from the
     loop, so a cancelled wait takes nothing)
   - `proc_open()` pipes and `proc_close()`; `popen()`/`pclose()`, `shell_exec()`
     (and backticks), `exec()`, `system()`, `passthru()` — both the output pipe
     and the wait for the child to exit; `pcntl_waitpid()`/`pcntl_wait()`
   - `STDIN`/`STDOUT`/`STDERR` and `php://stdin|stdout|stderr` reads and writes
     on pipes, sockets and ttys; in the CLI also `echo`/`print` to a stdout that
     would block (each echo stays contiguous, as natively)
   - `sleep()`, `usleep()`, `time_nanosleep()`, `time_sleep_until()`
   - `flock()`, `file_put_contents(..., LOCK_EX)`, `SplFileObject::flock()` on a
     held lock (retried without blocking, sleeping in the coroutine between
     tries: 1ms, doubling to 20ms — there is no descriptor to wait on)
   - DNS: `gethostbyname()`, `gethostbynamel()`, `gethostbyaddr()`,
     `dns_get_record()`, `checkdnsrr()`/`dns_check_record()`, `getmxrr()`/`dns_get_mx()`
     (via a thread pool)
   - regular-file reads that would wait for the disk, and reads and writes on
     network/FUSE mounts, via a thread pool: `fopen()`'d files, `file_get_contents()`, `file_put_contents()`, `file()`, `readfile()`,
     `fpassthru()`, `copy()`, `stream_copy_to_stream()`, `md5_file()`/`sha1_file()`/
     `hash_file()`, `SplFileObject` — never `include`/`require`, which must not
     suspend mid-compile; and on any disk `fsync()`/`fdatasync()` and FIFO `open()`
   - filesystem metadata and namespace calls on network/FUSE mounts: `stat()`,
     `lstat()`, `file_exists()`, `is_file()`/`is_dir()`/`is_link()`/`is_readable()`/
     `is_writable()`/`is_executable()`, `filesize()`/`filemtime()` & co.,
     `realpath()`, `readlink()`, `linkinfo()`, `scandir()`, `glob()`,
     `opendir()`/`dir()`, `unlink()`, `rename()`, `mkdir()`, `rmdir()`, `touch()`,
     `chmod()`/`chown()`/`chgrp()` & `l*` variants, `link()`, `symlink()`,
     `tempnam()`, `disk_free_space()`/`disk_total_space()`
     (via a thread pool; see below)

   The extension performs the real I/O and returns PHP's own results, errors
   and timeouts. This works because PHP fibers are stackful, so a suspend from
   inside `fread()`/`SSL_read()` unwinds and resumes correctly.

## Why

PHP's `stream_select()` fails (warning + `false`) once any watched descriptor exceeds
`FD_SETSIZE`, and PHP userland has no control over which fd number a stream gets —
so it is unreliable for programs juggling many streams. `Suspension::poll()` waits on epoll,
which has no such limit, and the extension adds the interception points a fiber
scheduler needs to make `mysqli`/PDO, `fread`/`fwrite`, `proc_open` and the sleep
functions transparently async. (The `stream_select()` limit itself is being lifted in
php-src, PR #14452.)

## Build

```sh
phpize
./configure --enable-phasync
make
make test
# load per-run in CLI:  php -d extension=modules/phasync.so your-app.php
```

It builds for PHP 8.3 to 8.5, on Linux and macOS. `phasync_arginfo.h` is maintained by hand
(`Makefile.frag` keeps make from regenerating it): never regenerate it with `gen_stub`.

## Install

**PIE** builds the extension and enables it in `php.ini`, for every SAPI:

```sh
pie install phasync/ext-phasync
```

**With phasync** — `composer require phasync/phasync` carries prebuilt binaries (PHP 8.3–8.5 NTS, Linux
x86_64/aarch64 glibc/musl, macOS arm64/x86_64) on its release tags. `vendor/bin/phasync script.php` runs
PHP with the matching one loaded, and `phasync\ext\ensure_loaded()` loads it into a running CLI
script. Both use an extension that is already installed (PIE, `php.ini`) as it is.

**By hand**, from anywhere on disk:

```sh
php -d extension=/path/to/phasync.so your-app.php
```

## API

```php
namespace phasync\ext;

final class Suspension extends \Exception {
    public static function manage(\Fiber $fiber): \Fiber;      // opt a fiber in; returns it
    public static function poll(array $suspensions, float $seconds): array;   // the ready ones, keys kept
    public static function readable(mixed $stream, ?float $timeout = null): bool;   // false: timed out
    public static function writable(mixed $stream, ?float $timeout = null): bool;
    public static function suspend(): void;                    // a blocking point for PHP code: ready at once
    public static function setPreemptInterval(?float $seconds): void;
    public function getScope(): \Scope;                        // the Scope current at the blocking call
    public function isReady(): bool;
    public function resume(): mixed;                           // as Fiber::resume()
    public function throw(\Throwable $exception): mixed;       // cancel the blocking call
}

function sapi(
    \Closure $code,             // one request; its return value is returned
    object   $sapi,             // where its response goes: name(), write(), send_headers(), ...
): mixed;

function run_next(string $script): void;   // CLI: run $script once more as a fresh request when this one ends

function dup2(mixed $stream, int $fd): void;   // as dup2(2): $fd stays open across requests (run_next()), owned by no PHP resource
```

```php
namespace phasync;

#[\Attribute(\Attribute::TARGET_FUNCTION | \Attribute::TARGET_METHOD)]
final class Uninterruptible {}
```

**Waiting.** A loop runs its coroutines as managed fibers:

```php
use phasync\ext\Suspension;

$fiber = Suspension::manage(new Fiber($task));
try {
    $fiber->start();
} catch (Suspension $s) {
    $pending[] = [$fiber, $s];       // the fiber is suspended in a blocking call
}
// later: wait for any of them, then resume the ready ones
foreach (Suspension::poll(array_column($pending, 1), $untilNextTimer) as $k => $s) {
    unset($pending[$k]);
    try { $s->resume(); } catch (Suspension $next) { $pending[] = [$fiber, $next]; }
}
```

`poll()` is the loop's one blocking call: it replaces the loop's
`stream_select()` and idle `usleep()`. It waits up to `$seconds` (0: don't wait;
negative: no limit) until one of the given Suspensions is ready, in one
`epoll_wait()`, costing work per ready event, never a scan of everything
registered. With none given it just waits (a loop sleeping until its next timer).
A signal ends the wait early once its handler has run, as with `stream_select()`,
so `poll()` may return fewer than asked for, or none: a loop polls again.
`resume()` on a Suspension that isn't ready, or was resumed already, throws a
`LogicException`. Outside a managed fiber every call behaves natively and no
Suspension is thrown. `poll()` called in a managed fiber is a blocking call like
any other, so a loop can run inside another loop's fiber.

A managed fiber may run fibers of its own (a library's, Drupal's renderer): a
blocking call in one of those suspends the nearest managed fiber, and the inner
fiber stays where it is meanwhile (it looks running; its `resume()` throws
`FiberError`). Resuming the managed fiber continues it.

A stream keeps its epoll registration, level-triggered and left armed after a
wait, one per direction: in request/response I/O nothing arrives while the
coroutine is busy, so a wait costs no syscall besides the batched
`epoll_wait()`. An event that finds nobody waiting disarms its direction then and
there, so unread data with nobody waiting costs one wake-up, never a busy loop.
`Suspension::readable()`/`writable()` on a regular file (always ready, and
refused by epoll) return at the loop's next `poll()`. Worker threads never call
PHP: the thread-pool work of hooked operations (file operations, DNS) queues its
Suspension and writes to an eventfd in the epoll set, which `poll()` drains.

A registration lives as long as its stream and is removed in the stream's close
op. A coroutine suspended in an operation on a stream (`fread()`,
`stream_socket_accept()`, `flock()`, a mysqli query, a read on the thread pool
...) that another coroutine closes has that operation fail as on a closed
descriptor (EBADF); the close waits in its coroutine until the operation has let
go of the stream. Closing it outside a coroutine meanwhile is a fatal error.
`socket_close()` likewise wakes a coroutine waiting on the `Socket`.

A close *begun* in a coroutine that is already being destroyed (its `finally`
blocks, PHP resuming it once to unwind it) cannot wait this way — it cannot
suspend. `fclose()`, the resource destructor, and `proc_close()`/`pclose()` of a
pipe handle this by waking the others and leaving the stream open under its last
remaining reference instead, which closes it for real once that reference goes;
no fatal error. `mysqli::close()` has no such adapter — mysqlnd frees its own
connection structures outside the streams API once its socket stream closes, so
a coroutine still inside a query on that connection has those structures freed
under it; this one case stays a fatal error, deliberately, rather than risk the
memory corruption a use-after-free there would be.

**Timeouts are PHP's, cancellation is the loop's.** When PHP's own call has a
timeout (a socket's `stream_set_timeout()` or `default_socket_timeout`, counted
per low-level wait as PHP counts it), its Suspension becomes ready when the time
runs out, and the call finishes exactly as native PHP does on a timeout — partial
data or `false`, `stream_get_meta_data()['timed_out']`, the notice for a
timed-out send. Where PHP would wait forever (pipes, files, FIFOs, pool
operations) so does the Suspension. `throw()` cancels: the exception propagates
out of the blocking call, after the extension has disarmed the wait, ready or
not.

**Suspensions are pooled.** The extension reuses one whose wait is over once
nothing else holds it (WeakMap entries and WeakReferences to it are cleared then),
and never buffers them as GC roots: a blocking call allocates nothing in the
steady state. A loop that drops a pending Suspension drops its fiber with it.

**The extension's safety doesn't depend on PHP code**, neither the application's
nor the loop's. Each coroutine has a ledger, in C, of what the extension holds
for it while it waits: its registration and Suspension, its place among a
stream's operations in flight, a pool operation, a closer's wait. The code after
the suspension settles it; if the fiber never gets back there, its destroy
observer does, and request shutdown settles what a fatal error left. A pool
operation owns its buffer and a duplicate of its descriptor, so a coroutine
destroyed meanwhile leaves it to its thread, which frees it. A coroutine closing
a stream others are inside an operation on waits for them, and is kept alive
meanwhile. A coroutine destroyed while it waits thus ends its operation safely;
what the operation returns is moot, since the fiber is unwinding. In its
`finally` blocks a wait fails with the `FiberError` PHP throws for
`Fiber::suspend()` there; pool operations run inline.

Descriptor-backed streams are **wrapped as soon as they are created**: sockets
from the tcp/unix/ssl transports (`stream_socket_client/server`, `fsockopen`),
`stream_socket_pair()`, `proc_open()` and `popen()` pipes, `fopen()` of regular files/FIFOs, and
the fd-backed `php://` streams (`php://stdin|stdout|stderr`, `php://fd/N`). Streams
that predate the hooks are swept up too — fds inherited across `ensure_loaded()`'s
re-exec are wrapped at request start, and the `STDIN`/`STDOUT`/`STDERR` constants
(which the CLI materialises lazily) at the first `Suspension::manage()`. A wrapped
stream is **indistinguishable from a raw one outside a managed fiber**: it blocks,
returns `EAGAIN`, and honours `stream_set_blocking()` exactly as an unwrapped
stream would. Only in a managed fiber, and only for a stream left in blocking
mode, does a would-block suspend the coroutine instead of blocking the process.

One coroutine at a time may wait on a stream in each direction: a second waiter
gets a `LogicException`, as with phasync's own pollers.

A minimal loop on this contract is in [`tests/loop.inc`](tests/loop.inc).

### Memcached

libmemcached does all its I/O on non-blocking sockets and waits in two places only:
`poll()` and, for a host name, `getaddrinfo()`. At startup, if `libmemcached.so` is
loaded, the extension points its own GOT entries for those two calls (Linux, x86_64
and aarch64) at replacements: in a managed fiber the replacement
suspends the coroutine until a descriptor is ready (a poll over several servers, as
`getMulti()` makes, is one wait) or the timeout is up, and `getaddrinfo()` runs on
the thread pool; anywhere else they are the libc calls. Nothing else changes, so
every method and the session handler behave as without the extension, and the
`OPT_POLL_TIMEOUT`, `OPT_CONNECT_TIMEOUT` and the other `OPT_*` settings, the result
codes and the messages are libmemcached's. A cancellation fails the call in
progress (`false`, with the loop's exception pending, which propagates).

The result code and message belong to the object, so read them before the
coroutine yields to another.

The wait for a locked session (`memcached.sess_locking`, `memcached.sess_lock_wait_*`;
phpredis' `redis.session.locking_enabled` likewise) is a `usleep()` retry loop in the
extension's C code. The extension points php-memcached's and phpredis' own `usleep()`
and `nanosleep()` at a sleep of the coroutine, so the request holding the lock, often
another coroutine of the same worker, runs meanwhile.

One limitation of the `memcached` session handler is not covered by the one-call
rule below, and cannot be from the extension (a fix belongs in php-memcached):
`memcached.sess_persistent=1` (off by default) makes the handler share one
persistent libmemcached handle without going through a `Memcached` method, so
concurrent coroutines can drive it at the same time and interleave on its socket.
`Suspension::manage()` refuses to run at all (a `LogicException`) while this is
combined with `session.save_handler=memcached`, rather than let it happen silently:
set `memcached.sess_persistent=0`, or use another session handler.

### PostgreSQL

libpq waits for its socket in `poll()` and resolves host names with
`getaddrinfo()`, both through `libpq.so`'s GOT, which the extension points at the
same replacements as for libmemcached. Connecting, the TLS handshake, queries,
`pg_sleep()` and `connect_timeout` all wait through the coroutine; results, errors
and notices are libpq's. Linux only; on macOS libpq still blocks the process.

### SQLite

At startup the extension registers a VFS wrapping SQLite's `unix` VFS as the
default. In a coroutine, the busy timeout's retries (`SQLite3::busyTimeout()`,
PDO's `ATTR_TIMEOUT`) sleep through the loop, so two connections to one database
wait for each other's locks without blocking the process; `fsync` runs on the
thread pool, and reads and writes too for a database on a network or FUSE mount
(the `phasync.fs_offload` rules). Locking, shared memory and mmap stay the `unix`
VFS's own. A connection opened with an explicit VFS keeps it.

### One call at a time per connection

A connection serves one call at a time: a second command written while the first
waits for its reply would interleave on its socket. So in a managed fiber a
coroutine entering a `Memcached` handle, a PostgreSQL connection (a
`PgSql\Connection`, its results and large objects, or the default link), a
`SQLite3` database (its statements and results) or a pgsql or sqlite `PDO` (its
statements) while another coroutine is in a call on it gets a `LogicException`:
"Concurrent operations on the same resource are not supported". Objects sharing
one persistent connection count as one. Calls made from the callbacks of a call on
the same connection nest. For `pg_*()` and `SQLite3` the check reads those extensions'
internals at layouts known for PHP 8.3-8.6; on another version it is off (stock
behaviour, waits still cooperative), and `phpinfo()` says so.

### Filesystem and DNS (thread pool)

Regular files and DNS lookups cannot be made non-blocking with readiness polling
(a disk fd is always "ready"; `getaddrinfo()` blocks). Like libuv/Node, these are
offloaded to a small worker thread pool: the worker runs only the raw syscall
(never the Zend engine, so it is safe in a non-ZTS build) and queues the waiting
coroutine's Suspension for `poll()` to find ready. Workers are pooled;
the pool size is the `phasync.thread_pool_size` INI (default 8; idle workers cost
only a lazily-paged stack, so a larger pool is cheap).

Filesystem metadata calls (`stat()`, `file_exists()`, `scandir()`, `unlink()`, …)
take about a microsecond on a local disk, and a pool round trip would make them
slower (tens of µs), which hurts code that makes thousands of them, like an
autoloader. So by default only paths on network or FUSE mounts (NFS, SMB/CIFS, 9p,
CephFS, virtiofs, sshfs and other `fuse.*` …, read from `/proc/self/mountinfo`) go
through the pool, where a call can stall for a network round trip or longer. The
`phasync.fs_offload` INI changes that: `network` (default), `all` (also for slow
local disks), or `none`. The mount table is checked for changes at most once a
second, so a new mount counts within a second and no filesystem call pays a
syscall for the check. Offloaded read-only calls stat or read the path on the
pool, warming the kernel's caches, and then run the original function, so
results, PHP's stat cache and warnings are exactly native.
`unlink()`/`rename()`/`mkdir()`/`rmdir()` run on the pool; if one fails, the
original function runs and reports the error with its usual warning.

Reads of regular files follow the same policy by the file's device, and on a
local disk they ask the kernel first: `preadv2()` with `RWF_NOWAIT` (Linux 4.14)
reads what is in the page cache, inline in one syscall, and says `EAGAIN` when it
would wait for the disk; only then does the read (or the rest of a partly cached
one) go to the pool. A cold read on a spinning disk then parks one coroutine for
the seek instead of stalling the worker. Some filesystems refuse `RWF_NOWAIT`,
ZFS among them (its ARC is not the page cache): their files read inline, as
natively, and the refusal is remembered per mount, so no read repeats it. Writes
stay inline on a local disk; a buffered write waits for the disk only under
writeback pressure.

`phasync.fs_offload_types` (comma-separated, default empty) lists filesystem types
whose regular-file reads and writes always go to the pool, as on a network mount:
`phasync.fs_offload_types=zfs` for ZFS on spinning disks. A cached read then costs
a pool round trip (tens of µs); metadata calls stay inline, since an autoloader
makes thousands and the kernel keeps them cached. It takes types rather than paths
because one type covers every mount of it (ZFS mounts each dataset on its own).

Named pipes (FIFOs) are the case cooperative scheduling *cannot* solve at all:
`open()` blocks in the kernel until the other end is opened, before any fd exists
to poll — so a single-threaded scheduler deadlocks unconditionally. Each FIFO
`open()` therefore runs on its own dedicated thread, letting a reader and a writer
coroutine rendezvous concurrently. After the open, a FIFO honours `O_NONBLOCK`, so
its reads/writes use the ordinary readiness path. If phasync times out or cancels
the coroutine, the extension `pthread_cancel`s the thread stuck in `open()`
(cancellation is scoped to that one syscall) and reaps it, so nothing leaks.

### Scope

`\Scope` (the Scope RFC) identifies one logical operation. `$scope->run($callable, ...$args)` runs a
callable in the scope; `Scope::getCurrent()` and `Scope::getRootScope()` return the current and the
root scope. A `Fiber` belongs to the scope that is current when it is constructed and a `Generator`
to the one current when it is created (`Fiber::getScope()`, `Generator::getScope()`); their code runs
in it wherever it is resumed from.

`Scope` itself is [fiber-interop/ext-scope](https://github.com/fiber-interop/ext-scope), bundled as is
inside `phasync.so`. If a standalone `scope` extension is already loaded, or a future PHP already has
a core `Scope` class, phasync yields to it instead of activating its own copy; `phpinfo()` says which
("bundled ext-scope", "provided by the scope extension", or "provided by PHP"). Nothing else in phasync
depends on which one is active.

### One request per coroutine: `sapi()`

`sapi($code, $sapi)` is the SAPI-facing side of a web server written in PHP: it runs `$code` (a
`\Closure`; a file path is not accepted) as a request. While it runs, and in every fiber started
inside it, `echo`, output buffering, `header()`, `headers_list()`, `http_response_code()`,
`setcookie()`, `php://input`, `request_parse_body()` and the session are that request's own, so many
requests interleave in one worker. A request's session is written and closed the way RSHUTDOWN
would when it returns or throws; two requests on the same session id serialise on the files
handler's lock cooperatively instead of blocking the worker. A userland session handler may
suspend too. Concurrent requests that use sessions need `sapi()` (Swerve uses it): outside it,
ext/session's state belongs to the whole process, whether or not the handler suspends. Nothing else is isolated: statics,
declarations, other globals, shutdown functions and error handlers are the worker's.

A request starts as it does under php-fpm: before `$code` runs, PHP's own request startup creates
`$_GET`, `$_POST`, `$_COOKIE`, `$_FILES`, `$_SERVER` and `$_REQUEST` from what `request_info()`
returns, with `variables_order`, `request_order`, `max_input_vars`, `post_max_size`,
`enable_post_data_reading` and the upload limits applied and their warnings reported as
"PHP Request Startup". A form POST's body is read before `$code` starts (up to `post_max_size`), so
parsing it never waits for the client. `filter_input()` sees the request's own input. The
superglobals are written into the current Scope (ext-scope keeps them per Scope), so a host runs each
`sapi()` call in its own Scope to keep them apart; the output, headers, body and session are the
request's own either way. `$_SESSION` follows the installed request the way ext/session keeps it in
step with `session_start()`.

What leaves the request goes to `$sapi` as a SAPI would receive it:

| Method | |
|---|---|
| `name(): string` | required; what `PHP_SAPI` and `php_sapi_name()` report inside the request |
| `write(string $data): bool` | required; the response body, piece by piece; `false`: the client is gone |
| `send_headers(int $status, ?string $statusLine, array $headers): void` | required; once, before the first output |
| `read(int $length): string` | the request body, `''` at its end |
| `request_info(): array` | `method`, `content_type`, `content_length`, `query_string`, `request_uri`, `cookie` (the raw `Cookie` header), `server` (the `$_SERVER` entries, beyond PHP's own) |
| `flush()`, `connection_aborted(): bool` | `flush()` and the client's departure |

`exit()` is not intercepted: it ends the script as under any SAPI, after the request's output has
reached `$sapi`. `write()` and `read()` may wait for the client, suspending the request. `write()`
returning `false` means the client is gone: `connection_aborted()` turns true and later output is
discarded; with `ignore_user_abort()` off (the default), the request stops right there -- no catch,
no finally, like `exit()` -- and `sapi()` returns `null` to its caller, which continues; other
requests running concurrently in the same worker (each `sapi()` call is its own coroutine) are
unaffected. Nesting throws. The full contract is the docblock in [`phasync.stub.php`](phasync.stub.php).

### Keywords: `run`, `begin`, `finish`

With `phasync.keywords=1` (`php.ini` or `-d`, not at run time) three words become structured-concurrency
syntax for phasync:

```php
$r = run function () { ... };      // \phasync::run(function () { ... })
$f = begin fetch($url);            // \phasync::go(fetch(...), $url)
$f = begin fn() => work($m);       // \phasync::go(fn() => work($m))
$body = finish $f;                 // \phasync::await($f)
```

`run` and `begin` take a closure literal or a call; in a call, the last call of the chain runs in the
coroutine, and its callee and arguments are evaluated where the keyword is (`begin $a->b()->c()` calls
`$a->b()` right away). `finish` binds like `clone`. The words are contextual: each is a keyword only
where PHP gives a syntax error today (`run(...)`, `Foo::begin`, `$o->finish`, a constant or a label
named so are untouched), so turning it on changes no working code.

The extension rewrites the tokens before the compiler, beneath opcache, so cached scripts cost
nothing more; the setting is part of opcache's system id. Line numbers stay. Static analysers, IDEs
and `nikic/php-parser` don't know the syntax. `vendor/bin/phasync` turns it on.

### Fresh request, same process: `run_next()`

`run_next($script)` makes the CLI process, once the current script has ended, run `$script` as a new
request in the same process: shutdown functions, destructors and output buffers of the first run
finish, then statics, globals, declarations, constants, ini values, handlers and output buffers start
over as in the first request, as under FPM. The opcache stays warm; no fork, no workers. A run that
calls `run_next()` again is followed by another, so a script can restart itself for as long as it
wants; the recording is used once. Whatever ended a run (`exit()`, a fatal error, an uncaught
exception), the next run still happens, and the process exits with the last run's status.

The working directory, umask, signal mask and `alarm()` are put back to what the process started
with. `STDIN`, `STDOUT` and `STDERR` are defined again; descriptors 0-2 and every inherited descriptor
stay open, so a socket passed as fd 3 can be served across runs: set `stream_set_read_buffer($s, 0)`
to read exactly what is consumed, since a stream's read buffer is lost with its request. Resource
limits are not reset, and a `fork()`ed child that ends does not run the next request. `$script` must be
readable when called (a `ValueError` otherwise), and the SAPI must be the CLI (an `Error`).

`dup2($stream, $fd)` puts `$stream`'s descriptor on `$fd` (as `dup2(2)`), owned by no PHP resource
afterwards, so it survives request shutdown and is there in every later `run_next()` run — the way a
`pcntl_fork()`ed worker keeps a channel to its parent on a fixed descriptor across restarts.

### Preemption: `Suspension::setPreemptInterval()`

`Suspension::setPreemptInterval($seconds)` suspends a managed fiber that has run
`$seconds` (wall clock) without suspending, between iterations of whatever PHP
loop it is running, with a Suspension that is ready at once: the loop gets
control back, runs its other coroutines, and resumes it, and the PHP loop goes on.
`null` stops it.

Preemption happens only between loop iterations, and not where the callback of
a C function or the engine may not suspend (`usort()` and kin, error and output
handlers, autoloaders, magic property methods, serialization hooks,
`preg_replace_callback()`, stream wrappers beyond plain I/O ...; the full list, with the reason for each, is in [docs/c-to-php-suspension.md](docs/c-to-php-suspension.md)), in
destructors, in exception handlers and shutdown functions, or in
`#[\phasync\Uninterruptible]` functions and what they call. The stack is checked down to the fiber's first
frame.

- Every backward jump (a loop's back-edge, a `foreach` `continue`, a backward
  `goto`) is compiled with a checkpoint before it:
  `if (\phasync\ext\__PREEMPT_DUE) \phasync\ext\checkpoint();`. Both are
  internal: the constant is a flag the extension flips (read-only to PHP code,
  not for use), and the function is in no function table on PHP 8.4+ (a
  frameless call; on 8.3 `function_exists()` sees it). The constant is
  registered per request, so opcache never folds it, and both JITs compile its
  read into loads and a compare, without a call; the call runs only when due
  and shows in no backtrace. A timer thread, not a signal, raises the flag, so
  no syscall fails with `EINTR` and the engine's interrupts are not used. The
  same in the interpreter and both JIT modes. A backward `goto` counts as a loop:
  the attribute is the way to make such code uninterruptible. Code without a loop
  (straight-line code, recursion) is never interrupted, nor is code the engine
  compiles without running extensions' compile hooks (`php -r`). A long C call
  delays it until it returns.
- `#[\phasync\Uninterruptible]` is not mutual exclusion: such a function can
  still suspend (on I/O, say), and other coroutines run, and are preempted,
  meanwhile.
- It is skipped where a fiber can't switch (pcntl signal handlers, a fiber being
  destroyed); the next interval gets it.
- The loop may `throw()` into a preempted fiber like into any other: the
  exception surfaces at the interrupted point.
- With no interval set there is no thread, and checkpoints only load the flag.
  Other interrupt users (`pcntl_async_signals()`, `max_execution_time`) keep
  working.
- It is per process (per thread on ZTS builds). A `fork()`ed child keeps the
  interval and gets a timer thread of its own.

A checkpoint the stack rules reject lowers the flag; the thread raises it again
0.25 ms later.

## Status

Sockets, TLS, pipes, processes, the sleep functions, DNS, files and filesystem
calls, flock and stdio are implemented and tested. Known TLS limitations:
read/write intent is approximated (TLS renegotiation wanting the opposite
direction could stall — the `stream_socket_get_crypto_status()` API on 8.5+ would
resolve it), and the TCP connect and DNS lookup inside a `tls://` connect still
run synchronously (ext/openssl calls the socket layer directly there); the
handshake itself cooperates.

Linux (epoll, eventfd) and macOS (kqueue); POSIX threads.

## License

MIT — see [LICENSE](LICENSE).
