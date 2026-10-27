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

echo "-- a call suspended in a fiber\n";
$log = [];
$stop = false;
$names = new WeakMap;
preempt_handler(function () use (&$log, $names) {
    if ($f = Fiber::getCurrent()) {
        $log[] = $names[$f];
        Fiber::suspend();
    }
}, 0.02);
$a = new Fiber(function () use (&$stop) { while (!$stop); });
$names[$a] = 'a';
$b = new Fiber(function () use (&$stop) { while (!$stop); });
$names[$b] = 'b';
$a->start();               // preempted: a's call is suspended
$b->start();               // b is preempted meanwhile
$a->resume();              // a's call returns, a is preempted again
$stop = true;
preempt_handler(null);
$a->resume();
$b->resume();
var_dump($log, $a->isTerminated(), $b->isTerminated());
?>
