--TEST--
run_next(): exit() in a run still lets the next registered run happen, and the last run's exit status is the process's
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/exit.php", <<<'PHP'
<?php
file_put_contents($argv[1], "x\n", FILE_APPEND);
$n = count(file($argv[1]));
echo "run $n\n";
if ($n < 3) {
    phasync\ext\run_next(__FILE__);
}
if ($n == 1) {
    exit(3);
}
if ($n == 2) {
    register_shutdown_function(function () { echo "exit from a shutdown function\n"; exit(4); });
    exit(5);
}
exit(7);
PHP);
[$out, $status] = rn_exec($d, "$d/exit.php", ["$d/state"]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
run 1
run 2
exit from a shutdown function
run 3
status: 7
