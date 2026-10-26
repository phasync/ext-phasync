<?php

/**
 * @generate-class-entries
 * @undocumentable
 */

namespace phasync\ext {

/**
 * Waiting for streams, for an event loop, on epoll.
 *
 * The loop gives it its slots: getSlot(): int returns a slot number no one else
 * uses; park(int $slot, float $timeout): void suspends the current coroutine in
 * the slot until unpark(), or until $timeout seconds pass (then it throws the
 * loop's timeout exception) or it is cancelled (then it throws that);
 * unpark(int $slot): bool resumes the coroutine parked in the slot, false if the
 * slot is vacant (its wait was cancelled or timed out).
 *
 * A wait takes a fresh slot and parks in it; the Poller unparks only on PHP's
 * thread, inside poll(). poll() also wakes the waiters of hooked operations
 * started under a manage() that was given this Poller (thread-pool work such as
 * file operations and DNS). The loop keeps its Poller alive: waiters parked
 * through a Poller that is freed are never unparked, so their waits run to their
 * timeouts. A Poller belongs to the process that created it: after fork(),
 * create a new one (using this one throws).
 *
 * @strict-properties
 * @not-serializable
 */
final class Poller
{
    public function __construct(\Closure $getSlot, \Closure $park, \Closure $unpark) {}

    /**
     * Wait up to $maxTime seconds (0: don't wait) until something waited on is
     * ready, unpark its waiters, and return: the loop's one blocking call.
     * poll(0) with nothing waited on and no finished thread task queued returns
     * without a syscall.
     */
    public function poll(float $maxTime): void {}

    /**
     * Park the current coroutine until $stream is readable, or at its end, or
     * failed (the next read then reports it the way PHP does). The loop's
     * exceptions (a timeout after $timeout seconds, a cancellation) propagate as
     * they are. One coroutine at a time may wait to read a stream (LogicException).
     *
     * @param resource $stream
     */
    public function readable(mixed $stream, float $timeout = \PHP_FLOAT_MAX): void {}

    /**
     * Park the current coroutine until $stream is writable, or closed, or failed,
     * as readable() does.
     *
     * @param resource $stream
     */
    public function writable(mixed $stream, float $timeout = \PHP_FLOAT_MAX): void {}
}

/**
 * Call $fn at most every $minInterval seconds (wall clock), between iterations
 * of whatever PHP loop is running then, so a scheduler can preempt a coroutine
 * that never yields: $fn may call Fiber::suspend(), and resuming the fiber
 * continues the loop.
 *
 * Preemption happens only between loop iterations, and not where the callback
 * of a C function or the engine may not suspend (docs/c-to-php-suspension.md
 * lists them: usort() and kin, error and output handlers, autoloaders, magic
 * property methods, serialization hooks, session handlers, stream wrappers
 * beyond plain I/O ...), nor in destructors, exception handlers, shutdown
 * functions, or #[\phasync\Uninterruptible] functions and what they call, down
 * to the fiber's first frame. Code without a loop is never
 * interrupted. A timer thread (not a signal: no syscall fails with EINTR) asks
 * the engine to interrupt; $fn runs once, without arguments. It is skipped
 * where suspending would throw FiberError (pcntl signal handlers, a fiber being
 * destroyed). It is never called while a call of it is still running in the
 * same fiber. An exception $fn throws surfaces at the interrupted point.
 *
 * Returns the previous closure. null removes it and stops the timer; with none
 * set there is no thread and no interrupt. It is per process (per thread on ZTS
 * builds), and a fork()ed child keeps it and has its own timer.
 */
function set_preempt_function(?\Closure $fn, float $minInterval = 0.1): ?\Closure {}

/**
 * Run $task with transparent fiber-async I/O active for its dynamic extent.
 *
 * While $task runs, blocking I/O inside a coroutine (fiber) parks it through
 * $poller instead of blocking the process, and the extension does the real I/O
 * once it may continue; the loop's $poller->poll() wakes it. $sleep(int
 * $microseconds) waits that long (a timer). Both are also called from a Fiber a
 * coroutine runs itself; the loop then runs in place in that Fiber until the
 * coroutine's wait is over, since suspending the Fiber would return to the
 * coroutine's own code.
 *
 * When PHP's own call has a timeout (a socket's stream_set_timeout() or
 * default_socket_timeout), the extension passes it to park(); if park() throws
 * $timeoutException once it has run out, the call finishes exactly as native PHP
 * does on a timeout (partial data or false, stream_get_meta_data()['timed_out'],
 * the notice for a timed-out send). Any other exception (a cancellation)
 * propagates out of the hooked function unchanged.
 *
 * Covers tcp/unix/ssl/tls sockets, stream_socket_pair(), proc_open()/popen()
 * pipes and process waits, STDIN/STDOUT/STDERR and echo in the CLI,
 * stream_select()/socket_select(), the sleep functions, DNS, file and
 * filesystem calls (through a worker thread pool) and flock(). A stream the
 * caller made non-blocking is never waited on: it behaves natively.
 *
 * Calls nest: an inner manage() sends hooked I/O to its own Poller until it
 * returns. Outside a coroutine, calls behave natively. Returns what $task returns.
 */
function manage(\Closure $task, Poller $poller, \Closure $sleep, string $timeoutException): mixed {}

/**
 * Run $code as one web request: it, and every fiber started inside it, have
 * their own output buffers, response headers and request body (php://input,
 * request_parse_body(), uploaded files), so many requests can run concurrently
 * in one worker. This is the SAPI-facing side of a web server written in PHP,
 * and nothing else: sapi() and \Scope are independent and compose. sapi() does
 * not create, enter or read a Scope, and touches no superglobal: the host writes
 * $_GET, $_POST, $_COOKIE, $_FILES, $_REQUEST and $_SERVER inside the Scope it
 * runs the request in (they are Scope state). Concurrent sapi() calls in one Scope, or in the root scope, are
 * fully independent for the SAPI state above. Nothing else is isolated: not
 * statics, declarations, other globals, the session, shutdown functions or
 * error handlers.
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
 * - request_info(): array: called once at the start: 'method', 'content_type'
 *   and 'content_length' of the request, as php://input and
 *   request_parse_body() need them, and 'query_string' and 'request_uri'.
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

}

namespace phasync {

/**
 * While a function with this attribute, or anything it calls in its fiber, runs,
 * the closure set with phasync\ext\set_preempt_function() is not called: it is
 * not preempted. It is not mutual exclusion: it can still suspend (on I/O, say),
 * and other coroutines run meanwhile.
 *
 * @strict-properties
 */
#[\Attribute(6)] // Attribute::TARGET_FUNCTION | Attribute::TARGET_METHOD
final class Uninterruptible
{
}

}

