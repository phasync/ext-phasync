--TEST--
Poller in select mode: a wait times out when nothing becomes ready, and the registration is left clean
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
$loop->runAll(function () use ($a, $loop, &$log) {
    $t = microtime(true);
    try {
        $loop->poller->readable($a, 0.1);
        $log[] = 'unexpected: became readable';
    } catch (LoopTimeout) {
        $elapsed = microtime(true) - $t;
        $log[] = 'timed out, elapsed_ok=' . ($elapsed >= 0.08 && $elapsed < 2.0 ? 'yes' : 'no');
    }
});
echo implode("\n", $log), "\n";

// A second wait on the same stream, through the same Poller, afterward still
// works natively: the timed-out registration was settled, not left armed.
fwrite($b, 'z');
$log2 = [];
$loop->runAll(function () use ($a, $loop, &$log2) {
    $loop->poller->readable($a);
    $log2[] = 'second wait: ' . fread($a, 1);
});
echo implode("\n", $log2), "\n";
fclose($a);
fclose($b);
?>
--EXPECT--
timed out, elapsed_ok=yes
second wait: z
