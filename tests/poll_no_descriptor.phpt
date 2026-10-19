--TEST--
Poller: waiting on a stream without a descriptor throws the ValueError that stream_select() throws (phasync/phasync#77)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
$loop = new Loop;
$m = fopen('php://memory', 'w+');

// What the pure-PHP driver does: stream_select() on a stream it cannot cast.
$r = [$m]; $w = $e = null;
try { @stream_select($r, $w, $e, 0); } catch (Throwable $x) { echo 'stream_select: ', get_class($x), ': ', $x->getMessage(), "\n"; }

$loop->runAll(function () use ($m, $loop) {
    foreach (['readable', 'writable'] as $method) {
        try { $loop->poller->$method($m); echo "returned\n"; }
        catch (Throwable $x) { echo "$method: ", get_class($x), ': ', $x->getMessage(), "\n"; }
    }
});
?>
--EXPECT--
stream_select: ValueError: No stream arrays were passed
readable: ValueError: No stream arrays were passed
writable: ValueError: No stream arrays were passed
