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
     * it is resumed (the fiber started in the scope current at its start()). Error without
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
