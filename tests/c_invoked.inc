<?php
// Two requests interleaved on tests/loop.inc's loop: A runs $op, which sleeps (a
// suspension point) inside $kind of PHP code; B logs while A would be suspended.
// Prints the log: "A before, B, A after" when A suspended, "A before, A after, B"
// when the sleep ran inline.
require_once __DIR__ . '/loop.inc';
require_once __DIR__ . '/sink.inc';

function c_invoked(string $kind, Closure $op): void
{
    $log = new ArrayObject;
    $loop = new Loop;
    $loop->runAll(
        function () use ($op, $log) {
            phasync\ext\sapi(function () use ($op, $log) {
                $log[] = 'A before';
                $op(fn () => usleep(20000));
                $log[] = 'A after';
            }, new Sink);
        },
        function () use ($log) {
            phasync\ext\sapi(function () use ($log) { $log[] = 'B'; }, new Sink);
        },
    );
    echo $kind, ': ', implode(', ', (array) $log), "\n";
}
