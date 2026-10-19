<?php
// A minimal event loop for the tests, on the extension's contract: slots
// (getSlot/park/unpark) given to a phasync\ext\Poller, a sleep timer, and the
// Poller's poll() as the one blocking call. It counts parks, and records the
// timeouts park() was given. A wait from a Fiber a coroutine runs itself
// (not a coroutine of this loop) drives the loop until the coroutine's wait is
// done: the Fiber stays where it is, other coroutines run meanwhile.

final class LoopTimeout extends Exception {}
final class LoopCancelled extends Exception {}

final class Loop
{
    public int $parks = 0;
    public array $parkTimeouts = [];   // what park() was given (null: none)
    public int $sleeps = 0;
    public array $sleepCalls = [];     // the microseconds asked for
    public bool $instantSleep = false; // record sleeps, don't wait
    public ?string $parkThrows = null; // park() throws this class at once
    public ?Closure $onPark = null;    // fn(int $slot, ?float $timeout): true = return at once (a spurious wake), or throw
    public bool $reverse = false;      // resume what became ready in reverse order
    public bool $spin = false;         // always poll(0): a loop that busy-polls
    public \phasync\ext\Poller $poller;

    private int $nextSlot = 0;
    private array $parked = [];        // slot => Fiber
    private array $deadlines = [];     // slot => microtime deadline
    private array $ready = [];         // [Fiber, 'start' | null | Throwable]
    private array $timers = [];        // [microtime, Fiber]
    private ?Fiber $current = null;    // the coroutine running
    private array $nested = [];        // spl_object_id(coroutine) => null waiting, true woken, or the Throwable it gets

    public function __construct()
    {
        $this->poller = new \phasync\ext\Poller($this->getSlot(...), $this->park(...), $this->unpark(...));
    }

    public function getSlot(): int
    {
        return $this->nextSlot++;
    }

    public function park(int $slot, float $timeout): void
    {
        if (isset($this->parked[$slot])) {
            throw new LogicException("Slot $slot is taken");
        }
        $this->parks++;
        $this->parkTimeouts[] = $timeout >= PHP_FLOAT_MAX ? null : $timeout;
        if ($this->parkThrows !== null) {
            throw new ($this->parkThrows)();
        }
        if ($this->onPark && ($this->onPark)($slot, end($this->parkTimeouts)) === true) {
            return;
        }
        $this->parked[$slot] = $this->current ?? Fiber::getCurrent();
        if ($timeout < PHP_FLOAT_MAX) {
            $this->deadlines[$slot] = microtime(true) + $timeout;
        }
        $this->wait();
    }

    /** Suspend the running coroutine, or, in a Fiber it runs itself, drive the loop until it is woken. */
    private function wait(): void
    {
        if ($this->current === null || Fiber::getCurrent() === $this->current) {   // null: a fiber the test runs itself
            Fiber::suspend();
            return;
        }
        $me = $this->current;
        $id = spl_object_id($me);
        $this->nested[$id] = null;
        try {
            while ($this->nested[$id] === null) {
                $this->step();
            }
        } finally {
            $how = $this->nested[$id];
            unset($this->nested[$id]);
            $this->current = $me;
        }
        if ($how instanceof Throwable) {
            throw $how;
        }
    }

    public function unpark(int $slot): bool
    {
        if (!isset($this->parked[$slot])) {
            return false;
        }
        $this->ready[] = [$this->parked[$slot], null];
        unset($this->parked[$slot], $this->deadlines[$slot]);
        return true;
    }

    public function sleep(int $microseconds): void
    {
        $this->sleeps++;
        $this->sleepCalls[] = $microseconds;
        if ($this->instantSleep) {
            return;
        }
        $this->timers[] = [microtime(true) + $microseconds / 1e6, $this->current ?? Fiber::getCurrent()];
        $this->wait();
    }

    /** Throw $e into a parked coroutine, as a cancellation does. */
    public function cancel(Fiber $fiber, Throwable $e = new LoopCancelled('cancelled')): void
    {
        foreach ($this->parked as $slot => $f) {
            if ($f === $fiber) {
                unset($this->parked[$slot], $this->deadlines[$slot]);
            }
        }
        foreach ($this->timers as $k => [, $f]) {
            if ($f === $fiber) {
                unset($this->timers[$k]);
            }
        }
        $this->ready = array_values(array_filter($this->ready, fn($r) => $r[0] !== $fiber));   // a wake-up queued already
        $this->ready[] = [$fiber, $e];
    }

    /** Forget a coroutine, as a loop dropping it does: its last reference may be the caller's. */
    public function drop(Fiber $fiber): void
    {
        foreach ($this->parked as $slot => $f) {
            if ($f === $fiber) {
                unset($this->parked[$slot], $this->deadlines[$slot]);
            }
        }
        foreach ($this->timers as $k => [, $f]) {
            if ($f === $fiber) {
                unset($this->timers[$k]);
            }
        }
        $this->ready = array_values(array_filter($this->ready, fn($r) => $r[0] !== $fiber));
    }

    public function go(Closure|Fiber $fn): Fiber
    {
        $fiber = $fn instanceof Fiber ? $fn : new Fiber($fn);
        $this->ready[] = [$fiber, 'start'];
        return $fiber;
    }

    /** Run until every coroutine is done (a watchdog fails a hung test instead of hanging it). */
    public function run(float $watchdog = 30): void
    {
        $this->end = microtime(true) + $watchdog;
        while ($this->step());
    }

    private float $end = PHP_FLOAT_MAX;

    /** One round: run what is ready, then poll. False when nothing is left. */
    private function step(): bool
    {
            while ($this->ready) {
                [$fiber, $how] = array_shift($this->ready);
                if (array_key_exists($id = spl_object_id($fiber), $this->nested)) {
                    $this->nested[$id] = $how ?? true;   // a coroutine waiting in a Fiber of its own: its wait is done
                    continue;
                }
                $prev = $this->current;
                $this->current = $fiber;
                try {
                    if ($how === 'start') {
                        $fiber->start();
                    } elseif ($how instanceof Throwable) {
                        $fiber->throw($how);
                    } else {
                        $fiber->resume();
                    }
                } finally {
                    $this->current = $prev;
                }
            }
            if (!$this->parked && !$this->timers) {
                return false;
            }
            if (microtime(true) > $this->end) {
                throw new RuntimeException('Loop watchdog: ' . count($this->parked) . ' coroutine(s) still parked');
            }
            $now = microtime(true);
            $next = $now + 0.5;
            foreach ($this->deadlines as $d) {
                $next = min($next, $d);
            }
            foreach ($this->timers as [$t]) {
                $next = min($next, $t);
            }
            $ready = count($this->ready);
            $this->poller->poll($this->spin ? 0 : max(0, $next - $now));
            if ($this->reverse) {
                $this->ready = array_merge(array_slice($this->ready, 0, $ready), array_reverse(array_slice($this->ready, $ready)));
            }
            $now = microtime(true);
            foreach ($this->timers as $k => [$t, $fiber]) {
                if ($t <= $now) {
                    unset($this->timers[$k]);
                    $this->ready[] = [$fiber, null];
                }
            }
            foreach ($this->deadlines as $slot => $d) {
                if ($d <= $now) {
                    $this->ready[] = [$this->parked[$slot], new LoopTimeout('timed out')];
                    unset($this->parked[$slot], $this->deadlines[$slot]);
                }
            }
            return true;
    }

    public function manage(Closure $task, string $timeoutClass = LoopTimeout::class): mixed
    {
        return \phasync\ext\manage($task, $this->poller, $this->sleep(...), $timeoutClass);
    }

    /** Run the coroutines inside manage() until all are done. */
    public function runAll(Closure|Fiber ...$coroutines): void
    {
        $this->manage(function () use ($coroutines) {
            foreach ($coroutines as $c) {
                $this->go($c);
            }
            $this->run();
        });
    }
}
