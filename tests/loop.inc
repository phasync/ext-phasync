<?php
// A minimal event loop for the tests, on the extension's contract: its coroutines are
// managed fibers (Suspension::manage()); a blocking call in one throws a Suspension out
// of the resume() that ran it, and Suspension::poll() is the loop's one blocking call.
// It counts the Suspensions it was given ($parks).

use phasync\ext\Suspension;

final class LoopTimeout extends Exception {}
final class LoopCancelled extends Exception {}

/** What tests used to reach through the loop's Poller: waits on streams, and one poll. */
final class LoopPoller
{
    public function __construct(private Loop $loop) {}

    /** Wait until $stream is readable; LoopTimeout once $timeout seconds have passed. */
    public function readable(mixed $stream, float $timeout = PHP_FLOAT_MAX): void
    {
        if (!Suspension::readable($stream, $timeout >= PHP_FLOAT_MAX ? null : $timeout)) {
            throw new LoopTimeout('timed out');
        }
    }

    public function writable(mixed $stream, float $timeout = PHP_FLOAT_MAX): void
    {
        if (!Suspension::writable($stream, $timeout >= PHP_FLOAT_MAX ? null : $timeout)) {
            throw new LoopTimeout('timed out');
        }
    }

    public function poll(float $maxTime): void
    {
        $this->loop->poll($maxTime);
    }
}

final class Loop
{
    public int $parks = 0;             // Suspensions caught
    public ?string $parkThrows = null; // throw this class into a coroutine as soon as it suspends
    public ?Closure $onPark = null;    // fn(Suspension $s): true = resume it at once (a spurious wake), or throw
    public bool $reverse = false;      // resume what became ready in reverse order
    public bool $spin = false;         // always poll(0): a loop that busy-polls
    public LoopPoller $poller;

    private array $pending = [];       // id => [Fiber, Suspension]
    private array $ready = [];         // [Fiber, 'start' | Suspension | [Suspension, Throwable]]
    private array $startArgs = [];     // spl_object_id(coroutine) => what go() passes to start()
    private float $end = PHP_FLOAT_MAX;
    private int $nextId = 0;

    public function __construct()
    {
        $this->poller = new LoopPoller($this);
    }

    /** Throw $e into a suspended coroutine, as a cancellation does. */
    public function cancel(Fiber $fiber, Throwable $e = new LoopCancelled('cancelled')): void
    {
        foreach ($this->pending as $id => [$f, $s]) {
            if ($f === $fiber) {
                unset($this->pending[$id]);
                $this->ready[] = [$fiber, [$s, $e]];
                return;
            }
        }
        foreach ($this->ready as $k => [$f, $how]) {
            if ($f === $fiber && $how instanceof Suspension) {
                $this->ready[$k] = [$fiber, [$how, $e]];
                return;
            }
        }
    }

    /** Forget a coroutine, as a loop dropping it does: its last reference may be the caller's. */
    public function drop(Fiber $fiber): void
    {
        foreach ($this->pending as $id => [$f]) {
            if ($f === $fiber) {
                unset($this->pending[$id]);
            }
        }
        $this->ready = array_values(array_filter($this->ready, fn($r) => $r[0] !== $fiber));
    }

    /** $args are passed to Fiber::start(): the coroutine's first frame is then a call from C with them. */
    public function go(Closure|Fiber $fn, mixed ...$args): Fiber
    {
        $fiber = Suspension::manage($fn instanceof Fiber ? $fn : new Fiber($fn));
        $this->ready[] = [$fiber, 'start'];
        $this->startArgs[spl_object_id($fiber)] = $args;
        return $fiber;
    }

    /** Run until every coroutine is done (a watchdog fails a hung test instead of hanging it). */
    public function run(float $watchdog = 30): void
    {
        $this->end = microtime(true) + $watchdog;
        while ($this->step());
    }

    /** One round: run what is ready, then poll. False when nothing is left. */
    private function step(): bool
    {
        while ($this->ready) {
            [$fiber, $how] = array_shift($this->ready);
            try {
                if ($how === 'start') {
                    $id = spl_object_id($fiber);
                    $args = $this->startArgs[$id] ?? [];
                    unset($this->startArgs[$id]);
                    $fiber->start(...$args);
                } elseif (is_array($how)) {
                    [$s, $e] = $how;
                    $s->throw($e);
                } else {
                    $how->resume();
                }
            } catch (Suspension $s) {
                $this->suspended($fiber, $s);
            }
        }
        if (!$this->pending) {
            return false;
        }
        if (microtime(true) > $this->end) {
            throw new RuntimeException('Loop watchdog: ' . count($this->pending) . ' coroutine(s) still suspended');
        }
        $this->poll($this->spin ? 0 : 0.5);
        return true;
    }

    private function suspended(Fiber $fiber, Suspension $s): void
    {
        $this->parks++;
        if ($this->parkThrows !== null) {
            $this->ready[] = [$fiber, [$s, new ($this->parkThrows)()]];
            return;
        }
        if ($this->onPark) {
            try {
                if (($this->onPark)($s) === true) {
                    $this->ready[] = [$fiber, $s];   // resumed before it is ready: it suspends again
                    return;
                }
            } catch (Throwable $e) {
                $this->ready[] = [$fiber, [$s, $e]];
                return;
            }
        }
        $this->pending[$this->nextId++] = [$fiber, $s];
    }

    /** Poll the pending Suspensions for at most $maxTime seconds; queue the ready ones. */
    public function poll(float $maxTime): void
    {
        $ready = Suspension::poll(array_map(fn($p) => $p[1], $this->pending), $maxTime);
        if ($this->reverse) {
            $ready = array_reverse($ready, true);
        }
        foreach ($ready as $id => $s) {
            $this->ready[] = [$this->pending[$id][0], $s];
            unset($this->pending[$id]);
        }
    }

    /** Kept for the tests' shape: the hooks follow the managed fibers, so this just runs $task. */
    public function manage(Closure $task): mixed
    {
        return $task();
    }

    /** Run the coroutines until all are done. */
    public function runAll(Closure|Fiber ...$coroutines): void
    {
        foreach ($coroutines as $c) {
            $this->go($c);
        }
        $this->run();
    }
}
