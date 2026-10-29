--TEST--
Suspension::writable(): writers on one blocking socket/pipe keep their large writes whole, in turn order
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
use phasync\ext\Suspension;

function drive(array $pair, int $n, string $label): void
{
    [$w, $r] = $pair;
    $loop = new Loop;
    $size = 256 * 1024;
    $got = '';
    $done = 0;
    $loop->go(function () use ($r, $n, $size, &$got) {
        $total = $n * ($size + 1);
        while (strlen($got) < $total) {
            Suspension::readable($r);
            $got .= fread($r, 65536);
            usleep(200);
        }
    });
    for ($i = 0; $i < $n; $i++) {
        $loop->go(function () use ($w, $i, $size, &$done) {
            fwrite($w, str_repeat(chr(65 + $i), $size) . "\n");
            $done++;
        });
    }
    $loop->run();
    $lines = explode("\n", rtrim($got, "\n"));
    $ok = count($lines) === $n;
    foreach ($lines as $i => $l) {
        $ok = $ok && strlen($l) === $size && $l === str_repeat(chr(65 + $i), $size);
    }
    echo $label, ': ', $done, ' writers, whole and in order: ', var_export($ok, true), "\n";
}
drive(stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP), 4, 'socket');
$d = [0 => ['pipe', 'r'], 1 => ['pipe', 'w']];
$proc = proc_open([PHP_BINARY, '-r', 'while (($c = fread(STDIN, 8192)) !== "" && $c !== false) echo $c;'], $d, $pipes);
drive([$pipes[0], $pipes[1]], 3, 'proc pipes');
fclose($pipes[0]); fclose($pipes[1]); proc_close($proc);
?>
--EXPECT--
socket: 4 writers, whole and in order: true
proc pipes: 3 writers, whole and in order: true
