<?php
// The preemption tests check where a running fiber can be interrupted; they were
// written against a callback. This keeps that shape on top of the Suspension
// contract: preempt_main() runs a test file (its globals intact) in a managed
// fiber and acts as its loop, and preempt_handler() sets the interval and the
// callback the loop runs whenever the fiber is preempted. Throwing from
// the callback throws into the fiber at the interrupted point; returning resumes
// it. Waits on I/O and timers are polled and resumed as usual.

use phasync\ext\Suspension;

final class PreemptTest
{
    public static ?\Closure $fn = null;
}

function preempt_handler(?\Closure $fn, float $minInterval = 0.1): ?\Closure
{
    $prev = PreemptTest::$fn;
    Suspension::setPreemptInterval($fn ? $minInterval : null);
    PreemptTest::$fn = $fn;
    return $prev;
}

function preempt_main(string $file): void
{
    $fiber = Suspension::manage(new \Fiber(fn() => \phasync\ext\include_global($file)));
    $resume = fn() => $fiber->start();
    for (;;) {
        try {
            $resume();
            return;
        } catch (Suspension $s) {
            if (!$s->isReady()) {
                Suspension::poll([$s], -1);
                $resume = fn() => $s->resume();
                continue;
            }
            try {
                if (PreemptTest::$fn) {
                    (PreemptTest::$fn)();
                }
                $resume = fn() => $s->resume();
            } catch (\Throwable $e) {
                $resume = fn() => $s->throw($e);
            }
        }
    }
}
