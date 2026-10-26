--TEST--
run_next(): no read of memory the CLI freed after its own request (valgrind)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!is_executable('/usr/bin/valgrind')) die('skip valgrind not installed');
?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/info.php", <<<'PHP'
<?php
echo getlastmod() > 0, getmyinode() > 0, get_current_user() !== '', getmyuid() >= 0, getmygid() >= 0, "\n";
file_put_contents($argv[1], "x\n", FILE_APPEND);
if (count(file($argv[1])) < 3) {
    phasync\ext\run_next(__FILE__);
}
PHP);
$ext = realpath(ini_get('extension_dir') . '/phasync.so');
$p = proc_open(['/usr/bin/valgrind', '-q', '--error-exitcode=99', PHP_BINARY, '-n', '-d', "extension=$ext", "$d/info.php", "$d/state"],
    [0 => ['file', '/dev/null', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, $d, ['USE_ZEND_ALLOC' => '0'] + getenv());
$out = stream_get_contents($pipes[1]);
echo $out, "status: ", proc_close($p), "\n";
rn_clean($d);
?>
--EXPECT--
11111
11111
11111
status: 0
