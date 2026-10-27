--TEST--
run_next(): an inherited socket (fd 3) is open in every lifecycle; unbuffered reads lose nothing, buffered reads may; EOF reaches the last lifecycle
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
// each lifecycle reads 5 bytes from fd 3; $argv[1] = 0: unbuffered, 1: the default buffering
file_put_contents("$d/main.php", <<<'PHP'
<?php
$state = __DIR__ . '/state';
$n = (int) @file_get_contents($state) + 1;
file_put_contents($state, $n);
$s = fopen('php://fd/3', 'r+');
if ($argv[1] === '0') {
    stream_set_read_buffer($s, 0);
}
echo "lifecycle $n: ", var_export(fread($s, 5), true), "\n";
if ($n < 4) {
    echo "next\n";
    phasync\ext\run_next(__FILE__);
}
PHP);
$run = function (string $buffered) use ($d) {
    @unlink("$d/state");
    [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
    $p = proc_open([PHP_BINARY, '-n', '-d', 'extension=' . realpath(ini_get('extension_dir') . '/phasync.so'), "$d/main.php", $buffered],
        [0 => ['file', '/dev/null', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1], 3 => $b], $pipes);
    fclose($b);
    fwrite($a, 'AAAAABBBBB');   // lifecycle 1 reads the first 5 bytes (and, buffered, all 10)
    $out = fgets($pipes[1]) . fgets($pipes[1]);
    fwrite($a, 'CCCCC');        // sent while lifecycle 2 starts
    stream_socket_shutdown($a, STREAM_SHUT_WR);   // then EOF (the child inherited $a too: closing it would not send one)
    $out .= fgets($pipes[1]) . fgets($pipes[1]) . fgets($pipes[1]);
    $out .= stream_get_contents($pipes[1]);
    return $out . 'status: ' . proc_close($p) . "\n";
};
echo "unbuffered\n", $run('0');
echo "buffered\n", $run('1');
rn_clean($d);
?>
--EXPECT--
unbuffered
lifecycle 1: 'AAAAA'
next
lifecycle 2: 'BBBBB'
next
lifecycle 3: 'CCCCC'
next
lifecycle 4: ''
status: 0
buffered
lifecycle 1: 'AAAAA'
next
lifecycle 2: 'CCCCC'
next
lifecycle 3: ''
next
lifecycle 4: ''
status: 0
