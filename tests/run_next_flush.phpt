--TEST--
run_next(): everything lifecycle 1 wrote (ob buffer, STDOUT, socket, plain file) is out before lifecycle 2 gets going
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", <<<'PHP'
<?php
$s = fopen('php://fd/3', 'r+');
if (!file_exists(__DIR__ . '/f')) {
    ob_start();
    echo "ob buffer\n";
    fwrite(STDOUT, "STDOUT\n");
    fwrite($s, "socket\n");
    $f = fopen(__DIR__ . '/f', 'w');
    fwrite($f, 'file');
    phasync\ext\run_next(__FILE__);
    return;
}
// lifecycle 2 waits for the parent, which reads lifecycle 1's output first
stream_set_read_buffer($s, 0);
echo "go: ", fread($s, 2), "; file: ", file_get_contents(__DIR__ . '/f'), "\n";
PHP);
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
$p = proc_open([PHP_BINARY, '-n', '-d', 'extension=' . realpath(ini_get('extension_dir') . '/phasync.so'), "$d/main.php"],
    [0 => ['file', '/dev/null', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1], 3 => $b], $pipes);
fclose($b);
echo fgets($a), fgets($pipes[1]), fgets($pipes[1]);   // blocks until lifecycle 1's output has been flushed
fwrite($a, 'go');
echo stream_get_contents($pipes[1]), 'status: ', proc_close($p), "\n";
rn_clean($d);
?>
--EXPECT--
socket
STDOUT
ob buffer
go: go; file: file
status: 0
