--TEST--
run_next(): descriptors received over the inherited socket (SCM_RIGHTS) work in every lifecycle and are closed with it
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); if (!is_file(PHP_EXTENSION_DIR . '/sockets.so')) die('skip ext-sockets is not available');
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", <<<'PHP'
<?php
$state = __DIR__ . '/state';
$n = (int) @file_get_contents($state) + 1;
$sock = socket_import_stream(fopen('php://fd/3', 'r+'));
$msg = ['name' => [], 'buffer_size' => 1, 'controllen' => socket_cmsg_space(SOL_SOCKET, SCM_RIGHTS, 1)];
socket_recvmsg($sock, $msg, 0);
echo "lifecycle $n: ", var_export(fread($msg['control'][0]['data'][0], 100), true), "\n";
$count = count(scandir('/proc/self/fd'));
if ($n > 2) {   // the first lifecycle has descriptors 0-2 as streams, later ones dups
    echo $count === (int) file_get_contents($state . 'c') ? "fd count flat\n" : "fd count changed\n";
}
file_put_contents($state, $n);
file_put_contents($state . 'c', $count);
if ($n < 3) {
    echo "next\n";
    phasync\ext\run_next(__FILE__);
}
PHP);
file_put_contents("$d/send.php", <<<'PHP'
<?php
$sock = socket_import_stream(fopen('php://fd/3', 'r+'));
$f = fopen(__DIR__ . '/' . $argv[1], 'r');
socket_sendmsg($sock, ['iov' => ['x'], 'control' => [['level' => SOL_SOCKET, 'type' => SCM_RIGHTS, 'data' => [$f]]]], 0);
PHP);
file_put_contents("$d/one", 'first file');
file_put_contents("$d/two", 'second file');
file_put_contents("$d/three", 'third file');
$ext = 'extension=' . PHP_EXTENSION_DIR . '/sockets.so';
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
$send = fn(string $f) => proc_close(proc_open([PHP_BINARY, '-n', '-d', $ext, "$d/send.php", $f], [3 => $a], $none));
$p = proc_open([PHP_BINARY, '-n', '-d', 'extension=' . realpath(ini_get('extension_dir') . '/phasync.so'), '-d', $ext, "$d/main.php"],
    [0 => ['file', '/dev/null', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1], 3 => $b], $pipes);
$send('one');
echo fgets($pipes[1]), fgets($pipes[1]);
$send('two');
echo fgets($pipes[1]), fgets($pipes[1]);
$send('three');
echo stream_get_contents($pipes[1]), 'status: ', proc_close($p), "\n";
rn_clean($d);
?>
--EXPECT--
lifecycle 1: 'first file'
next
lifecycle 2: 'second file'
next
lifecycle 3: 'third file'
fd count flat
status: 0
