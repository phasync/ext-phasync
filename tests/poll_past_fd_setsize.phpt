--TEST--
Poller: waits on a stream whose descriptor is past the FD_SETSIZE ceiling (select() cannot)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
$lim = @shell_exec('bash -lc "ulimit -n" 2>/dev/null');
if ((int)$lim < 3000) die("skip needs ulimit -n >= 3000");
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
$pairs = [];
for ($i = 0; $i < 1500; $i++) {
    $p = @stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
    if (!$p) break;
    $pairs[] = $p;
}
if (count($pairs) < 1100) die("skip could not open enough pairs");
$last = $pairs[count($pairs) - 1];       // descriptors well past 1024

$loop = new Loop;
$loop->runAll(
    function () use ($last, $loop) { $loop->poller->readable($last[1]); echo 'readable: ', fread($last[1], 1), "\n"; },
    function () use ($last) { usleep(50000); fwrite($last[0], 'x'); },
);
$loop = new Loop;
$loop->runAll(function () use ($last, $loop) { $loop->poller->writable($last[1]); echo "writable\n"; });
?>
--EXPECT--
readable: x
writable
