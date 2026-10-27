--TEST--
Poller: waits parked under select() when the switch to epoll happens (sockets, a timeout, a close, a pool completion) each wake
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
if ((int) @shell_exec('ulimit -n') < 4000) die('skip needs ulimit -n >= 4000');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
$n = 1200;   // past PHASYNC_SELECT_THRESHOLD watched descriptors
$loop = new Loop;
$file = tempnam(sys_get_temp_dir(), 'pss');
file_put_contents($file, str_repeat('z', 300000));
$early = [];
for ($i = 0; $i < 20; $i++) {
    $early[] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
}
[$silent] = $quiet = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
$late = [];
$got = 0;
$log = [];
$co = [];
foreach ($early as $k => [$r]) {
    $co[] = function () use ($r, $k, $loop, &$got, &$log) {
        $loop->poller->readable($r, 5);
        if ($k === 0) {
            $log[] = 'closed one woke: ' . var_export(is_resource($r), true);
            return;
        }
        $got += fread($r, 1) === 'x';
    };
}
$co[] = function () use ($silent, $loop, &$log) {
    try {
        $loop->poller->readable($silent, 0.3);
        $log[] = 'silent: woke?!';
    } catch (LoopTimeout) {
        $log[] = 'silent: timed out';
    }
};
$co[] = function () use ($file, &$log) {
    $log[] = 'pool read: ' . strlen(file_get_contents($file));
};
$co[] = function () use ($n, $loop, &$late, &$got, $early) {
    usleep(20000);   // the early waits are parked under select()
    for ($i = 0; $i < $n; $i++) {
        $late[] = $p = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        $loop->go(function () use ($p, $loop, &$got) {
            $loop->poller->readable($p[0], 5);
            $got += fread($p[0], 1) === 'x';
        });
    }
    usleep(20000);   // switched to epoll, everything parked
    fclose($early[0][0]);
    foreach (array_slice($early, 1) as [, $w]) {
        fwrite($w, 'x');
    }
    foreach ($late as [, $w]) {
        fwrite($w, 'x');
    }
};
$loop->runAll(...$co);
sort($log);
echo implode("\n", $log), "\n";
echo "woke with data: ", $got === 19 + $n ? 'all' : $got, "\n";
unlink($file);
?>
--EXPECT--
closed one woke: false
pool read: 300000
silent: timed out
woke with data: all
