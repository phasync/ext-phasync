<?php

/**
 * @generate-class-entries
 * @undocumentable
 */

namespace phasync\ext {

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
 * This request's phasync.gc_grace counters: 'grace_ms' (0 when off),
 * 'collections', 'hidden' (objects whose children a collection skipped
 * because they were younger than the grace), 'held' (of those, still
 * waiting to age), 'rerooted' (put back in the root buffer once old) and
 * 'scan_ns' (time spent on this bookkeeping).
 *
 * @return array{grace_ms: int, collections: int, hidden: int, held: int, rerooted: int, scan_ns: int}
 */
function gc_grace_stats(): array {}

/**
 * The stat cache's counters (phasync.stat_cache; docs/stat-cache.md): 'hits' (answered without a
 * syscall), 'misses' (looked up and remembered), 'fallbacks' (not cacheable: PHP's own call),
 * 'flushes' (everything dropped: a watched directory changed, the watch cap, a queue overflow),
 * 'events' (inotify events read), and the current 'watches' and 'entries'. Process-wide.
 *
 * @return array{enabled: bool, hits: int, misses: int, fallbacks: int, flushes: int, events: int, watches: int, entries: int}
 */
function stat_cache_stats(): array {}

}
