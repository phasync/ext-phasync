--TEST--
run_next(): under a SAPI other than the CLI it throws an Error
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/router.php", <<<'PHP'
<?php
try {
    phasync\ext\run_next(__FILE__);
} catch (Error $e) {
    echo get_class($e), ": ", $e->getMessage();
}
PHP);
$s = stream_socket_server('tcp://127.0.0.1:0');
$port = parse_url(stream_socket_get_name($s, false), PHP_URL_PORT);
fclose($s);
$so = realpath(ini_get('extension_dir') . '/phasync.so');
$p = proc_open([PHP_BINARY, '-n', '-d', "extension=$so", '-S', "127.0.0.1:$port", "$d/router.php"], [1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
for ($i = 0; $i < 100 && !($c = @fsockopen('127.0.0.1', $port)); $i++) {
    usleep(50000);
}
fclose($c);
echo file_get_contents("http://127.0.0.1:$port/"), "\n";
proc_terminate($p);
proc_close($p);
rn_clean($d);
?>
--EXPECT--
Error: phasync\ext\run_next() requires the CLI SAPI (this is cli-server)
