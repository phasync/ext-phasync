--TEST--
Poller in select mode: a thread-pool task's eventfd wake-up alongside a socket wait
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--INI--
phasync.fs_offload=all
--FILE--
<?php
require __DIR__ . '/loop.inc';
$dir = sys_get_temp_dir() . '/phasync_pw_' . bin2hex(random_bytes(4));
mkdir($dir);
$data = str_repeat('x', 200000);
file_put_contents("$dir/src", $data);
$expectedHash = md5($data);

[$r, $w] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
$loop = new Loop;
$log = [];
$before = $loop->parks;
// The file read is offloaded to the thread pool, which reports back through
// the Poller's completion channel (an eventfd), the same channel that's
// always in the select() read set alongside the socket below -- both must
// wake correctly out of a single select() mode poll(), with only one real
// registration (the socket) ever in the Poller.
$loop->runAll(
    function () use ($dir, $expectedHash, &$log) {
        $hash = md5(file_get_contents("$dir/src"));
        $log[] = 'file: ' . ($hash === $expectedHash ? 'match' : 'MISMATCH');
    },
    function () use ($r, $loop, &$log) {
        $loop->poller->readable($r);
        $log[] = 'socket: ' . fread($r, 1);
    },
    function () use ($w) {
        usleep(10000);
        fwrite($w, 'y');
    },
);
sort($log);
echo implode("\n", $log), "\n";
var_dump($loop->parks > $before);
fclose($r);
fclose($w);
?>
--EXPECT--
file: match
socket: y
bool(true)
