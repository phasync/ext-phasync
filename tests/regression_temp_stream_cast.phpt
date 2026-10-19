--TEST--
Regression: php://temp/memory streams in a fiber inside a scope: Poller waits (which cast the stream) and >2MB spills keep working (no segfault, data intact)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
// Casting php://temp to an fd spills its buffer into a temporary stdio file,
// written inside the cast; the file must stay a plain stdio stream (php://temp
// checks php_stream_is(STDIO) on it) or the next cast crashed. Reported with
// phasync::io(), whose driver waits on such streams. Poller::readable()/writable()
// cast the stream the same way.
require __DIR__ . '/loop.inc';
$loop = new Loop;
$code = function () use ($loop) {
    $t = fopen('php://temp', 'w+');
    fwrite($t, 'hello');
    for ($i = 0; $i < 3; $i++) {                 // repeated casts
        $loop->poller->readable($t);
        $loop->poller->writable($t);
        echo "poller wait: ok\n";
    }
    $r = [$t]; $w = []; $e = null;
    echo 'native select: ', stream_select($r, $w, $e, 0), "\n";
    rewind($t);
    echo 'data: ', stream_get_contents($t), "\n";

    $m = fopen('php://memory', 'w+');            // can't be cast: ValueError, no crash
    try { $loop->poller->readable($m); } catch (ValueError $x) { echo "memory: ValueError\n"; }

    $big = fopen('php://temp/maxmemory:1024', 'w+');   // spills on write
    fwrite($big, str_repeat('x', 5000));
    fwrite($big, 'end');
    rewind($big);
    echo 'spilled: ', strlen(stream_get_contents($big)), "\n";
};
$loop->runAll($code);
?>
--EXPECT--
poller wait: ok
poller wait: ok
poller wait: ok
native select: 1
data: hello
memory: ValueError
spilled: 5003
