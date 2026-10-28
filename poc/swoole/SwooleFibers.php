<?php

declare(strict_types=1);

use Swoole\Coroutine as Co;

/**
 * Pseudo-fiber driver for Swoole: each task is a Swoole coroutine (no Zend fiber inside it), and the
 * \Fiber handed out for it is a pseudo-fiber whose methods land here.
 *
 *  - Fiber::suspend($v) in a task: Co::yield(); $fiber->resume($v) / throw($e): Co::resume($cid).
 *  - start()/resume() return the next Fiber::suspend() value, or null when the task instead blocked in a
 *    Swoole-hooked call (it then continues on its own, like a fiber parked by phasync's loop).
 *  - throw() into a task blocked in a hooked call cancels that call, and the extension swaps Swoole's
 *    CanceledException for $e. A wait Swoole can't interrupt (a FIFO open) throws $e when it ends.
 */
final class SwooleFibers
{
    /** @var WeakMap<Fiber, stdClass> */
    private WeakMap $tasks;
    /** @var array<int, WeakReference> cid => Fiber */
    private array $byCid = [];

    public function __construct()
    {
        $this->tasks = new WeakMap();
        \phasync\pseudo_fiber_driver($this);
    }

    public function create(callable $fn): Fiber
    {
        $fiber = \phasync\pseudo_fiber();
        $this->tasks[$fiber] = (object) ['fn' => $fn, 'status' => 'init', 'cid' => 0, 'out' => null, 'in' => null,
            'ret' => null, 'exc' => null, 'excSeen' => false, 'pending' => null];

        return $fiber;
    }



    public function count(): int
    {
        return count($this->tasks);
    }

    // ---- driver methods, called by the extension ----

    public function start(Fiber $fiber, array $args): mixed
    {
        $t = $this->task($fiber);
        if ('init' !== $t->status) {
            \phasync\fiber_error('Cannot start a fiber that has already been started');
        }
        $t->status = 'running';
        $ref = WeakReference::create($fiber);
        Co::create(function () use ($t, $ref, $args) {
            $t->cid = Co::getCid();
            $this->byCid[$t->cid] = $ref;
            try {
                $t->ret = ($t->fn)(...$args);
            } catch (Throwable $e) {
                $t->exc = $e;
            } finally {
                $t->status = 'terminated';
                $t->fn = null;
                unset($this->byCid[$t->cid]);
            }
        });

        return $this->settle($t);
    }

    public function resume(Fiber $fiber, mixed $value): mixed
    {
        return $this->enter($fiber, [false, $value]);
    }

    public function throw(Fiber $fiber, Throwable $e): mixed
    {
        $t = $this->task($fiber);
        if ('running' === $t->status && $t->cid !== Co::getCid()) {
            if (!\phasync\swoole_cancel($t->cid, $e)) {   // blocked in a hooked call: it throws $e there
                $t->pending = $e;                             // otherwise the next park throws it
            }

            return $this->settle($t);
        }

        return $this->enter($fiber, [true, $e]);
    }

    public function getReturn(Fiber $fiber): mixed
    {
        $t = $this->task($fiber);
        if ('terminated' !== $t->status) {
            \phasync\fiber_error('init' === $t->status ? 'Cannot get fiber return value: The fiber has not been started' : 'Cannot get fiber return value: The fiber has not returned');
        }
        if ($t->exc) {
            \phasync\fiber_error('Cannot get fiber return value: The fiber threw an exception');
        }

        return $t->ret;
    }

    public function status(Fiber $fiber): string
    {
        return $this->task($fiber)->status;
    }

    public function current(): ?Fiber
    {
        return ($this->byCid[Co::getCid()] ?? null)?->get();
    }

    public function suspend(mixed $value): mixed
    {
        $fiber = $this->current() ?? \phasync\fiber_error('Cannot suspend outside of fiber');
        $t = $this->tasks[$fiber];
        unset($fiber);   // a parked task must not keep its own handle alive
        if ($t->pending) {
            $e = $t->pending;
            $t->pending = null;
            throw $e;
        }
        $t->status = 'suspended';
        $t->out = $value;
        Co::yield();
        [$isThrow, $v] = $t->in;
        $t->in = null;
        $t->status = 'running';
        if ($isThrow) {
            throw $v;
        }

        return $v;
    }

    // ----

    private function task(Fiber $fiber): stdClass
    {
        return $this->tasks[$fiber] ?? \phasync\fiber_error('Not a pseudo-fiber of this driver');
    }

    private function enter(Fiber $fiber, array $in): mixed
    {
        $t = $this->task($fiber);
        if ('suspended' !== $t->status) {
            \phasync\fiber_error('Cannot resume a fiber that is not suspended');
        }
        $t->status = 'running';
        $t->in = $in;
        Co::resume($t->cid);

        return $this->settle($t);
    }

    /** After the task gave control back: its suspend value, its exception if it just died, else null. */
    private function settle(stdClass $t): mixed
    {
        if ('suspended' === $t->status) {
            $v = $t->out;
            $t->out = null;

            return $v;
        }
        if ('terminated' === $t->status && $t->exc && !$t->excSeen) {
            $t->excSeen = true;
            throw $t->exc;
        }

        return null;
    }
}
