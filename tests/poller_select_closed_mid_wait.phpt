--TEST--
Poller in select mode: a stream closed while watched (its own close, and its peer's)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';

// A: the watched stream itself is fclose()'d by another coroutine. Only one
// descriptor is ever registered here, so this runs well under the
// select/epoll switching threshold.
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
$log = [];
$loop = new Loop;
$loop->runAll(
    function () use ($a, $loop, &$log) {
        $t = microtime(true);
        $loop->poller->readable($a, 5);
        $log[] = 'A: woke fast=' . ((microtime(true) - $t) < 1.0 ? 'yes' : 'no');
    },
    function () use ($a, &$log) {
        usleep(50000);
        fclose($a);
        $log[] = 'B: closed A';
    },
);
echo implode("\n", $log), "\n";
fclose($b);

// C: the PEER is closed instead -- select() reports EOF as read-ready on its
// own, the same way epoll's EPOLLRDHUP/EPOLLHUP does; no special case is
// needed for it in select mode.
[$c, $d] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
$log2 = [];
$loop2 = new Loop;
$loop2->runAll(
    function () use ($c, $loop2, &$log2) {
        $loop2->poller->readable($c, 5);
        $log2[] = 'C: read=' . var_export(fread($c, 10), true);
    },
    function () use ($d) {
        usleep(50000);
        fclose($d);
    },
);
echo implode("\n", $log2), "\n";
fclose($c);
?>
--EXPECT--
B: closed A
A: woke fast=yes
C: read=''
