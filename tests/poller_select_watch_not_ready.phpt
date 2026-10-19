--TEST--
Poller in select mode: stream_select() on a stream another coroutine reads waits for a close or its own readiness only, never for the free direction (no busy loop)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
$loop = new Loop;
$log = [];
$loop->runAll(
    function () use ($a, $loop, &$log) {
        $loop->poller->readable($a, 5);
        $log[] = 'A: readable';
    },
    function () use ($a, &$log) {
        $r = [$a]; $w = $e = null;
        $n = stream_select($r, $w, $e, 0, 300000);
        $log[] = "B: $n";
    },
    function () use ($b, &$log) {
        usleep(400000);
        fwrite($b, 'x');
    },
);
echo implode("\n", $log), "\n";
echo $loop->parks < 20 ? "few parks\n" : "busy: {$loop->parks} parks\n";
?>
--EXPECT--
B: 0
A: readable
few parks
