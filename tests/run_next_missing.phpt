--TEST--
run_next(): a script that does not exist or cannot be read throws at the call
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
mkdir("$d/dir");
file_put_contents("$d/main.php", <<<'PHP'
<?php
foreach (['nope.php', __DIR__ . '/dir'] as $s) {
    try {
        phasync\ext\run_next($s);
    } catch (Throwable $e) {
        echo get_class($e), ": ", str_replace(__DIR__, 'DIR', $e->getMessage()), "\n";
    }
}
echo "done\n";
PHP);
[$out, $status] = rn_exec($d, "$d/main.php");
echo $out, "status: $status\n";
rmdir("$d/dir");
rn_clean($d);
?>
--EXPECT--
ValueError: phasync\ext\run_next(): Argument #1 ($script) must be a readable file, "DIR/nope.php" is not
ValueError: phasync\ext\run_next(): Argument #1 ($script) must be a readable file, "DIR/dir" is not
done
status: 0
