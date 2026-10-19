--TEST--
Poller in select mode: a waited descriptor closed past the close hooks (a user stream's cast) wakes its waiter instead of failing poll()
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
class W {
    public static $inner;
    public $context;
    function stream_open($p, $m, $o, &$op) { return true; }
    function stream_cast($as) { return self::$inner; }
    function stream_read($n) { return false; }
    function stream_eof() { return true; }
}
stream_wrapper_register('ww', 'W');
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
W::$inner = $a;
$loop = new Loop;
$log = [];
$loop->runAll(
    function () use ($loop, &$log) {
        $u = fopen('ww://x', 'r');   // inside manage(): not wrapped by its sweep (#87)
        $t = microtime(true);
        $loop->poller->readable($u, 5);
        $log[] = 'A: woke fast=' . (microtime(true) - $t < 1.0 ? 'yes' : 'no');
    },
    function () use ($a, &$log) {
        usleep(50000);
        fclose($a);
        $log[] = 'B: closed the inner stream';
    },
);
echo implode("\n", $log), "\n";
?>
--EXPECT--
B: closed the inner stream
A: woke fast=yes
