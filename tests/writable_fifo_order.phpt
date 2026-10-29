--TEST--
Suspension::writable(): waiters on one stream are woken oldest first, one per readiness
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

[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
$loop = new Loop;
$log = [];
foreach (['1', '2', '3'] as $n) {
    $loop->go(function () use ($a, &$log, $n) {
        Suspension::writable($a);
        $log[] = $n . 'a';
        Suspension::writable($a);
        $log[] = $n . 'b';
    });
}
$loop->run();
echo implode(' ', $log), "\n";

[$a, $b, $filled] = full_pair();
$loop = new Loop;
$log = [];
foreach (['1', '2', '3'] as $n) {
    $loop->go(waiter($loop, $log, $a, $n));
}
$loop->go(function () use ($b, $filled, &$log) { $log[] = 'drain'; drain($b, $filled); });
$loop->run();
echo implode(' ', $log), "\n";
?>
--EXPECT--
1a 2a 3a 1b 2b 3b
drain 1 ready 2 ready 3 ready
