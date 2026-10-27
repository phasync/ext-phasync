--TEST--
run_next(): the script runs again after the current one ends, for as long as each run registers the next
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/loop.php", <<<'PHP'
<?php
file_put_contents($argv[1], "x\n", FILE_APPEND);
$n = count(file($argv[1]));
echo "lifecycle $n of ", basename($argv[0]), "\n";
if ($n < 3) {
    phasync\ext\run_next(__FILE__);
}
PHP);
[$out, $status] = rn_exec($d, "$d/loop.php", ["$d/state"]);
echo $out, "status: $status\n";
echo count(file("$d/state")), " lifecycles\n";
rn_clean($d);
?>
--EXPECT--
lifecycle 1 of loop.php
lifecycle 2 of loop.php
lifecycle 3 of loop.php
status: 0
3 lifecycles
