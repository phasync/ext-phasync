<?php

function spin(float $s): void { $t = microtime(true); while (microtime(true) - $t < $s); }

echo "-- a slow closure\n";
$depth = 0;
$max = 0;
$calls = 0;
preempt_handler(function () use (&$depth, &$max, &$calls) {
    $calls++;
    $max = max($max, ++$depth);
    spin(0.3);             // many intervals pass in here
    $depth--;
}, 0.02);
spin(0.7);
preempt_handler(null);
var_dump($max, $calls >= 1 && $calls <= 3);

echo "-- managed fibers preempted in turn\n";
use phasync\ext\Suspension;
$log = [];
$stop = false;
Suspension::setPreemptInterval(0.02);
$a = Suspension::manage(new Fiber(function () use (&$stop) { while (!$stop); }));
$b = Suspension::manage(new Fiber(function () use (&$stop) { while (!$stop); }));
$step = function (string $name, Closure $run) use (&$log): ?Suspension {
    try {
        $run();
        return null;
    } catch (Suspension $s) {
        $log[] = $name;
        return $s;
    }
};
$sa = $step('a', fn() => $a->start());     // preempted
$sb = $step('b', fn() => $b->start());     // b is preempted meanwhile
$sa = $step('a', fn() => $sa->resume());   // a is preempted again
$stop = true;
Suspension::setPreemptInterval(null);
$sa->resume();
$sb->resume();
var_dump($log, $a->isTerminated(), $b->isTerminated());
?>
