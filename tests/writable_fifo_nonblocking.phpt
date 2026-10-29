--TEST--
Suspension::writable(): a non-blocking stream gets no write turn, and a second reader still throws
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

[$a, $b, $filled] = full_pair();
stream_set_blocking($a, false);
$loop = new Loop;
$r = [];
$loop->go(function () use ($a, &$r) { $r[] = 'nb1 ' . var_export(@fwrite($a, str_repeat('n', 65536)), true); });
$loop->go(function () use ($a, &$r) { $r[] = 'nb2 ' . var_export(@fwrite($a, str_repeat('n', 65536)), true); });
$loop->run();
echo implode(', ', $r), "\n";

// A short count on a non-blocking stream while a writable() waiter is queued: still unchanged.
$loop = new Loop;
$r = [];
$loop->go(function () use ($a, &$r) { Suspension::writable($a); $r[] = 'w1'; });
$loop->go(function () use ($a, &$r) { Suspension::writable($a); $r[] = 'w2'; });
$loop->go(function () use ($a, $b, $filled, &$r) { $r[] = 'nb ' . var_export(@fwrite($a, 'zz'), true); drain($b, $filled); });
$loop->run();
echo implode(', ', $r), "\n";

// readable() keeps its one-reader rule.
$loop = new Loop;
$r = [];
$loop->go(function () use ($b, &$r) { Suspension::readable($b, 0.2); $r[] = 'r1 done'; });
$loop->go(function () use ($b, &$r) {
    try { Suspension::readable($b, 0.2); } catch (LogicException $e) { $r[] = $e->getMessage(); }
});
$loop->run();
echo implode(', ', $r), "\n";
?>
--EXPECT--
nb1 0, nb2 0
nb 0, w1, w2
Another coroutine is already waiting to read from this stream, r1 done
