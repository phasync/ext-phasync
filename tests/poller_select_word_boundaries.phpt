--TEST--
Poller in select mode: descriptors at the bitmap's word boundaries (63, 64, 127, 128, ...) and large unaligned ones wake their own waiters only
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
if (PHP_OS_FAMILY !== 'Linux') die('skip needs /proc/self/fd');
if ((int) @shell_exec('ulimit -n') < 30000) die('skip needs ulimit -n >= 30000');
?>
--INI--
memory_limit=-1
--FILE--
<?php
require __DIR__ . '/loop.inc';
// Descriptors are allocated lowest-free: find the next free ones from /proc/self/fd
// and fill with dummies up to each target.
$hold = [];
$used = [];
foreach (scandir('/proc/self/fd') as $f) {
    if (is_numeric($f) && @readlink("/proc/self/fd/$f") !== false) {   // the listing's own handle is closed by now
        $used[(int) $f] = true;
    }
}
$next = 0;
$take = function () use (&$used, &$next) {
    while (isset($used[$next])) $next++;
    $used[$next] = true;
    return $next;
};
$pairs = [];
foreach ([63, 127, 191, 1023, 4095, 20003, 20063] as $t) {
    while (true) {
        $save = $next;
        $fd = $take();
        if ($fd >= $t) { unset($used[$fd]); $next = $save; break; }
        $hold[] = fopen('/dev/null', 'r');
    }
    $fd = $take();
    $take();
    $pairs[$fd] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
}
echo implode(',', array_filter(array_keys($pairs), fn ($fd) => str_starts_with(readlink("/proc/self/fd/$fd"), 'socket:')
    && str_starts_with(readlink('/proc/self/fd/' . ($fd + 1)), 'socket:'))), "\n";
$loop = new Loop;
$log = [];
$co = [];
$i = 0;
foreach ($pairs as $fd => $pair) {
    $silent = $i++ % 3 === 2;   // these never get data: must time out, not wake
    foreach ($pair as $k => $s) {
        $co[] = function () use ($fd, $k, $s, $silent, $loop, &$log) {
            $n = $fd + $k;
            try {
                $loop->poller->readable($s, $silent ? 0.3 : 5);
                $log[$n] = "$n:" . fread($s, 10);
            } catch (LoopTimeout) {
                $log[$n] = "$n:timeout";
            }
        };
        if (!$silent) {
            $co[] = function () use ($fd, $k, $pair) {
                usleep(30000);
                fwrite($pair[1 - $k], 'x' . ($fd + $k));
            };
        }
    }
}
$loop->runAll(...$co);
ksort($log);
echo implode(' ', $log), "\n";
?>
--EXPECT--
63,127,191,1023,4095,20003,20063
63:x63 64:x64 127:x127 128:x128 191:timeout 192:timeout 1023:x1023 1024:x1024 4095:x4095 4096:x4096 20003:timeout 20004:timeout 20063:x20063 20064:x20064
