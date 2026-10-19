--TEST--
Poller in select mode: multiple descriptors past FD_SETSIZE waited on concurrently, one of them timing out
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
for ($i = 0; $i < 1300; $i++) {
    $p = @stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
    if (!$p) break;
    $pairs[] = $p;
}
if (count($pairs) < 1200) die("skip could not open enough pairs");

// Three descriptors well past the 1024 FD_SETSIZE ceiling, waited on at once:
// each needs its own bit in a bitmap grown to fit the highest one by hand
// (the libc FD_SET/FD_ISSET macros abort past FD_SETSIZE under FORTIFY).
$a = $pairs[1000];
$b = $pairs[1100];
$c = $pairs[1199];
$log = [];
$loop = new Loop;
$loop->runAll(
    function () use ($a, $loop, &$log) {
        $loop->poller->readable($a[1]);
        $log[] = 'a: ' . fread($a[1], 1);
    },
    function () use ($b, $loop, &$log) {
        $loop->poller->readable($b[1]);
        $log[] = 'b: ' . fread($b[1], 1);
    },
    function () use ($c, $loop, &$log) {
        try {
            $loop->poller->readable($c[1], 0.2);
            $log[] = 'c: unexpected ready';
        } catch (LoopTimeout) {
            $log[] = 'c: timed out';
        }
    },
    function () use ($a, $b) {
        usleep(30000);
        fwrite($a[0], 'A');
        fwrite($b[0], 'B');
        // $c is never written: it must time out on its own, while a/b become
        // select()-ready in the same past-FD_SETSIZE bitmap.
    },
);
sort($log);
echo implode("\n", $log), "\n";
?>
--EXPECT--
a: A
b: B
c: timed out
