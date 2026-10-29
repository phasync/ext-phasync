--TEST--
Suspension::writable(): closing the stream wakes every waiter, each then gets the normal error
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
$loop = new Loop;
$log = [];
foreach ([1, 2, 3] as $n) {
    $loop->go(function () use ($a, &$log, $n) {
        Suspension::writable($a);
        $log[] = "$n woke: " . (is_resource($a) ? 'open' : 'closed');
    });
}
$loop->go(function () use ($a) { fclose($a); });
$loop->run();
echo implode("\n", $log), "\n";

// fwrite()s queued behind a turn holder: each fails as PHP does on a closed stream.
[$a, $b, $filled] = full_pair();
$loop = new Loop;
$log = [];
foreach ([1, 2, 3] as $n) {
    $loop->go(function () use ($a, &$log, $n) {
        try {
            fwrite($a, str_repeat('y', 65536));
            $log[] = "$n returned";
        } catch (Throwable $e) {
            $log[] = "$n " . get_class($e) . ': ' . $e->getMessage();
        }
    });
}
$loop->go(function () use ($a) { fclose($a); });
$loop->run();
echo implode("\n", $log), "\n";
?>
--EXPECT--
1 woke: closed
2 woke: closed
3 woke: closed
1 returned
2 returned
3 returned
