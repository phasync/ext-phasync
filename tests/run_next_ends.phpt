--TEST--
run_next(): a run that does not register ends the process; a fatal error in it gives a non-zero status
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/once.php", <<<'PHP'
<?php
echo "once\n";
PHP);
file_put_contents("$d/first.php", <<<'PHP'
<?php
echo "first\n";
phasync\ext\run_next($argv[1]);
PHP);
file_put_contents("$d/fatal.php", <<<'PHP'
<?php
echo "fatal run\n";
undefined_function();
PHP);
file_put_contents("$d/first_fatal.php", <<<'PHP'
<?php
echo "first\n";
phasync\ext\run_next($argv[1]);
exit(2);
PHP);
echo "plain:\n";
[$out, $status] = rn_exec($d, "$d/once.php");
echo $out, "status: $status\n";
echo "registers once:\n";
[$out, $status] = rn_exec($d, "$d/first.php", ["$d/once.php"]);
echo $out, "status: $status\n";
echo "then a fatal error:\n";
[$out, $status] = rn_exec($d, "$d/first_fatal.php", ["$d/fatal.php"]);
echo preg_replace('/PHP Fatal.*\n/', '', $out), "status: $status\n";
rn_clean($d);
?>
--EXPECT--
plain:
once
status: 0
registers once:
first
once
status: 0
then a fatal error:
first
fatal run
Fatal error: Uncaught Error: Call to undefined function undefined_function() in DIR/fatal.php:3
Stack trace:
#0 {main}
  thrown in DIR/fatal.php on line 3
status: 255
