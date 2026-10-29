--TEST--
Suspension::writable(): a cancelled, timed-out or destroyed waiter leaves the queue and passes its turn on
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/writable_fifo.inc';
use phasync\ext\Suspension;

// Cancelled: the front waiter, then one in the middle.
foreach ([1, 2] as $victim) {
    [$a, $b, $filled] = full_pair();
    $loop = new Loop;
    $log = [];
    $fibers = [];
    foreach ([1, 2, 3] as $n) {
        $fibers[$n] = $loop->start(waiter($loop, $log, $a, (string) $n));
    }
    $loop->cancel($fibers[$victim]);
    $loop->go(function () use ($b, $filled) { drain($b, $filled); });
    $loop->run();
    echo "cancel $victim: ", implode(', ', $log), "\n";
}

// Timed out in the middle.
[$a, $b, $filled] = full_pair();
$loop = new Loop;
$log = [];
$loop->go(waiter($loop, $log, $a, '1'));
$loop->go(waiter($loop, $log, $a, '2', 0.1));
$loop->go(waiter($loop, $log, $a, '3'));
$loop->go(function () use ($b, $filled) { usleep(300000); drain($b, $filled); });
$loop->run();
echo 'timeout: ', implode(', ', $log), "\n";

// Destroyed: the front waiter, and one in the middle.
foreach (['front', 'middle'] as $which) {
    [$a, $b, $filled] = full_pair();
    $log = [];
    $loop = new Loop;
    $other = function (string $n) use ($loop, &$log, $a) { return waiter($loop, $log, $a, $n); };
    $before = $which === 'front' ? [] : [$other('1')];
    $alongside = $which === 'front' ? [$other('2'), $other('3')] : [$other('3')];
    destroy_while_waiting($loop, 'refcount', fn() => Suspension::writable($a), after: [function () use ($b, $filled) { drain($b, $filled); }], before: $before, alongside: $alongside);
    echo "destroy $which: ", implode(', ', $log), "\n";
}
?>
--EXPECT--
cancel 1: 1 LoopCancelled, 2 ready, 3 ready
cancel 2: 2 LoopCancelled, 1 ready, 3 ready
timeout: 2 timeout, 1 ready, 3 ready
  finally ran
destroy front: 2 ready, 3 ready
  finally ran
destroy middle: 1 ready, 3 ready
