<?php

/**
 * @generate-class-entries
 * @undocumentable
 */

namespace phasync\ext {

/**
 * How a blocking call in a fiber reaches an event loop, instead of blocking the
 * process.
 *
 * A loop opts its fibers in with manage(). When code in a managed fiber, or in an
 * unmanaged fiber it runs, makes a blocking call (sockets and TLS, pipes and
 * processes, STDIN/STDOUT/STDERR and echo in the CLI, stream_select() and
 * socket_select(), the sleep functions, DNS, file and filesystem calls through a
 * worker thread pool, flock(), curl, PDO pgsql, memcached ...), the extension
 * suspends the nearest managed fiber and throws a Suspension out of the
 * start()/resume()/throw() that ran it. The loop keeps it, waits for it with
 * poll(), and resumes it with resume() once it is ready; the call then completes
 * in the fiber exactly as it would have natively, with PHP's own results, errors
 * and timeouts. Outside a managed fiber every call behaves natively, and a
 * Suspension is never thrown.
 *
 * An unmanaged fiber the managed one runs (a library's own) stays where it is
 * while the managed fiber is suspended: it looks running (its resume() throws
 * FiberError), and resuming the managed fiber continues it.
 *
 * Suspensions are pooled by the extension: one whose wait is over is reused once
 * nothing else holds it, and WeakMap entries and WeakReferences to it are cleared
 * then. A loop that drops a pending Suspension drops its fiber with it.
 *
 * @strict-properties
 * @not-serializable
 */
final class Suspension extends \Exception
{
    private function __construct() {}

    /**
     * Wait up to $seconds (0: don't wait; negative: no limit) until at least one of
     * $suspensions is ready, and return the ready ones with their keys. A wait that
     * timed out (a socket's native timeout, a sleep) counts as ready. With none
     * given it just waits (a loop sleeping until its next timer). A signal ends the
     * wait early, after its handler has run, as stream_select() does. Called in a
     * managed fiber, it is a blocking call like any other: it suspends that fiber
     * with a Suspension that is ready when one of these is, so a loop can run inside
     * another loop's fiber.
     *
     * @param array<int|string, Suspension> $suspensions
     * @return array<int|string, Suspension>
     */
    public static function poll(array $suspensions, float $seconds): array {}

    /** Opt $fiber in: its blocking calls suspend it with a Suspension. Returns $fiber. */
    public static function manage(\Fiber $fiber): \Fiber {}

    /**
     * Wait until $stream is readable, or at its end, or failed (the next read then
     * reports it as PHP does): suspends the managed fiber, or blocks outside one.
     * Returns at once when the stream has buffered data. False if $timeout seconds
     * passed first.
     *
     * @param resource $stream
     */
    public static function readable(mixed $stream, ?float $timeout = null): bool {}

    /**
     * Wait until $stream is writable, or closed, or failed, as readable() does.
     *
     * @param resource $stream
     */
    public static function writable(mixed $stream, ?float $timeout = null): bool {}

    /**
     * A blocking point for PHP code (a library's own wait: a sleep, a lock, a
     * channel): suspends the nearest managed fiber, from anywhere in the fibers it
     * runs, with a Suspension that is ready at once. The loop resumes it, or throws
     * into it, when the wait is over. LogicException outside a managed fiber.
     */
    public static function suspend(): void {}

    /**
     * Preempt a managed fiber that runs $seconds without suspending, at its next
     * checkpoint (between loop iterations), with a Suspension that is ready at once.
     * Not where a fiber can't switch (docs/c-to-php-suspension.md lists them),
     * nor in destructors, exception handlers, shutdown functions, or
     * #[\phasync\Uninterruptible] functions and what they call. A timer thread asks
     * for it (not a signal: no syscall fails with EINTR). null stops it.
     */
    public static function setPreemptInterval(?float $seconds): void {}

    /**
     * The scope current at the blocking call: the one the fiber continues in when
     * it is resumed (Fiber::getScope() is the scope it belongs to). Error without
     * the Scope class, or once it was resumed.
     */
    public function getScope(): \Scope {}

    /**
     * The managed fiber this Suspension suspended, so a loop that polls
     * Suspensions needs no fiber table of its own. null once the wait is over
     * (resumed, thrown into, or the fiber resumed directly): the Suspension no
     * longer holds the fiber.
     */
    public function getFiber(): ?\Fiber {}

    /** True once poll() has found it ready; resume() may be called then. */
    public function isReady(): bool {}

    /**
     * Resume the fiber: its blocking call completes. Returns what Fiber::resume()
     * returns, and throws the fiber's next Suspension. LogicException if it isn't
     * ready, or was resumed already.
     */
    public function resume(): mixed {}

    /**
     * Throw $exception into the fiber at its blocking call, which is abandoned (a
     * cancellation); ready or not. LogicException if it was resumed already.
     */
    public function throw(\Throwable $exception): mixed {}
}

/**
 * Run $code as one web request: it, and every fiber started inside it, have
 * their own output buffers, response headers, session and request body (php://input,
 * request_parse_body(), uploaded files), so many requests can run concurrently
 * in one worker. This is the SAPI-facing side of a web server written in PHP.
 * register_shutdown_function() inside $code registers for the request: those
 * run in registration order, in the request, when $code returns, throws or
 * exit()s, before its output is flushed, its headers sent and its session
 * written. An exception or exit() in one skips the rest and leaves sapi() as
 * one from $code would. Nothing else is isolated: not statics, declarations,
 * other globals or error handlers.
 *
 * Before $code runs, PHP's own request startup creates $_GET, $_POST, $_COOKIE,
 * $_FILES, $_SERVER and $_REQUEST from request_info(), as under php-fpm:
 * variables_order, request_order, max_input_vars, post_max_size,
 * enable_post_data_reading and the upload limits apply, and their warnings are
 * reported as "PHP Request Startup". A form POST's body is read before $code
 * starts (up to post_max_size). filter_input() sees the request's own input.
 * The superglobals go into the current \Scope, so concurrent sapi() calls need a
 * Scope each to keep them apart; everything else above is the request's own in
 * any Scope.
 *
 * $code is a \Closure: a file path or any other callable is a TypeError.
 *
 * header(), headers_sent(), headers_list(), http_response_code(), setcookie()
 * and php://input work on the request's own. PHP_SAPI and php_sapi_name() are
 * the name $sapi->name() gives, inside the request. is_uploaded_file() and
 * move_uploaded_file() work for the files request_parse_body() parses.
 *
 * What would go to the client goes to $sapi, as a SAPI would receive it:
 *
 * - name(): string (required): the SAPI's name, as php_sapi_name() reports it
 *   inside the request.
 * - write(string $data): bool (required): output leaving the output buffers, piece
 *   by piece, as a SAPI's ub_write gets it. false means the client is gone:
 *   connection_aborted() becomes true and later output is discarded. With
 *   ignore_user_abort() off (the default), $code stops right there -- no catch,
 *   no finally, like exit() -- and sapi() returns null to its caller, which
 *   continues; other requests running concurrently (each sapi() call is its own
 *   coroutine) are unaffected. With ignore_user_abort(true), $code runs on.
 * - send_headers(int $status, ?string $statusLine, array $headers): void
 *   (required): once, before the first output or when the request ends without
 *   any. $headers are raw lines as headers_list() returns them; $statusLine is
 *   set by header('HTTP/1.1 ...').
 * - flush(): void: flush() was called.
 * - read(int $length): string: up to $length bytes of the request body, '' at
 *   its end, when the code reads php://input or calls request_parse_body();
 *   called again until it has enough or gets ''. Without it, the request has no
 *   body. php://input is blocking under every SAPI (stream_set_blocking()
 *   changes nothing), so a read() that has to wait suspends the request.
 * - request_info(): array: called once at the start: 'method', 'content_type',
 *   'content_length', 'query_string' and 'request_uri' of the request, 'cookie'
 *   (the raw Cookie header) and 'server' (an array of $_SERVER entries; PHP adds
 *   its own, such as REQUEST_TIME, and nothing from the worker's environment).
 * - connection_aborted(): bool: asked by connection_aborted() and
 *   connection_status(), for a server that knows the client left.
 *
 * Methods other than the three required are used only if present. They run
 * outside the request (their own output goes where it would without sapi()),
 * in the Scope the sapi() call was made in. write() and read() may wait for the
 * client, suspending the request's coroutine; the others are called from inside
 * PHP's request handling and should not wait.
 *
 * When $code returns or throws, the output buffers are flushed (removable or
 * not) and the headers are sent if nothing sent them. An uncaught exception
 * propagates. exit() is not intercepted: it ends the script as it does under any
 * SAPI, after the request's output has been flushed to $sapi. Fibers of the
 * request still running afterwards have their output discarded. Nesting throws
 * an Error.
 */
function sapi(\Closure $code, object $sapi): mixed {}

/**
 * Run $script once more, as a new request, when the current script has ended:
 * after its shutdown functions, destructors and output buffers, the process runs
 * $script (a path, resolved against the working directory now) from a fresh
 * request start, in the same process. Statics, globals, declared functions and
 * classes, constants, ini values, handlers, output buffers and registered
 * shutdown functions are as in the first request of a process, as under FPM;
 * the opcache stays warm; $argv, $_SERVER and the process id are those of the
 * first. The working directory, umask, signal mask and alarm() are put back to
 * what the process started with (a working directory it cannot return to ends
 * the process with status 255).
 *
 * The recording is used once: the new run is the one to call run_next() again
 * if it is to be followed by another, and it can do so forever. Calling it again
 * before the script ends replaces the recording. Whatever ends a run, including
 * exit() and a fatal error, the next run still happens if one was recorded
 * before. The exit status of the process is the last run's. A fork()ed child
 * that ends does not run it. Descriptors the process inherited stay open (a
 * stream's read buffer is lost with its request: set it to 0 to read exactly
 * what is consumed); resource limits are not reset. Throws a ValueError
 * if $script is not a readable file, an Error unless the SAPI is the CLI or on a
 * system without ELF binaries (macOS, Windows).
 */
function run_next(string $script): void {}

/**
 * Duplicate the file descriptor of $stream onto $fd, as dup2(2): $fd is closed first if open,
 * and afterwards belongs to no PHP resource, so it stays open across requests (run_next())
 * until the process closes it or exits. $stream itself is unchanged.
 *
 * @param resource $stream a stream with a file descriptor (a socket, a file, a pipe)
 * @throws \ValueError if $fd is negative
 * @throws \TypeError if $stream is not a stream that can give a file descriptor
 * @throws \RuntimeException if dup2() fails (with strerror)
 */
function dup2(mixed $stream, int $fd): void {}

/**
 * Runs $file as PHP runs a script's main file: its top-level code executes in
 * the global scope, so its variables are globals (seen by `global $x` and
 * $GLOBALS), even when called from inside a function, a closure or a Fiber.
 * Otherwise like require: an uncaught exception or a compile error propagates
 * to the caller, the return value is $file's, opcache and include_path apply,
 * and __FILE__/__DIR__ are $file's.
 *
 * Throws an Error while other code runs in the global scope with top-level
 * variables of its own, such as the main script or another include_global()
 * still running in another Fiber: both would own the same variables.
 */
function include_global(string $file): mixed {}

/**
 * Process-wide counters of de-blocked pipe I/O: 'pipe_nowait' counts a pipe
 * read/write that used preadv2()/pwritev2() with RWF_NOWAIT (no O_NONBLOCK
 * toggle); 'pipe_fcntl_fallback' counts one that fell back to toggling
 * O_NONBLOCK around the syscall, which only happens if the running kernel
 * lacks RWF_NOWAIT support. Never reset.
 *
 * @return array{pipe_nowait: int, pipe_fcntl_fallback: int}
 */
function io_stats(): array {}

/**
 * The memo's counters (phasync.memo_memory; README, Memo): hits per function, 'misses',
 * 'stored', 'evicted', 'refused' (not stored: too large, or the graveyard full until the
 * request ends), 'entries', 'bytes' (what the entries take), 'graveyard_bytes' (evicted,
 * freed when the request ends) and 'limit'. Process-wide, never reset.
 *
 * @return array{enabled: bool, json_hits: int, file_hits: int, unserialize_hits: int, dir_hits: int, misses: int, stored: int, evicted: int, refused: int, entries: int, bytes: int, graveyard_bytes: int, limit: int}
 */
function memo_stats(): array {}

/**
 * The stat cache's counters (phasync.stat_cache; docs/stat-cache.md): 'hits' (answered without a
 * syscall), 'misses' (looked up and remembered), 'fallbacks' (not cacheable: PHP's own call),
 * 'flushes' (everything dropped: a watched directory changed, the watch cap, a queue overflow),
 * 'events' (inotify events read), and the current 'watches' and 'entries'. Process-wide.
 *
 * @return array{enabled: bool, hits: int, misses: int, fallbacks: int, flushes: int, events: int, watches: int, entries: int}
 */
function stat_cache_stats(): array {}
 * This request's phasync.gc_grace counters: 'grace_ms' (0 when off),
 * 'collections', 'hidden' (objects whose children a collection skipped
 * because they were younger than the grace), 'held' (of those, still
 * waiting to age), 'rerooted' (put back in the root buffer once old) and
 * 'scan_ns' (time spent on this bookkeeping).
 *
 * @return array{grace_ms: int, collections: int, hidden: int, held: int, rerooted: int, scan_ns: int}
 */
function gc_grace_stats(): array {}

}

namespace phasync {

/**
 * While a function with this attribute, or anything it calls in its fiber, runs,
 * Suspension::setPreemptInterval() does not preempt it. It is not mutual exclusion: it can still suspend (on I/O, say),
 * and other coroutines run meanwhile.
 *
 * @strict-properties
 */
#[\Attribute(6)] // Attribute::TARGET_FUNCTION | Attribute::TARGET_METHOD
final class Uninterruptible
{
}

}
