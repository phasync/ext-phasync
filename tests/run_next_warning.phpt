--TEST--
run_next(): warnings in later lifecycles are not prefixed "PHP Shutdown:"
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", <<<'PHP'
<?php
if (!file_exists(__DIR__ . '/m')) {
    touch(__DIR__ . '/m');
    phasync\ext\run_next(__FILE__);
    exit;
}
fopen('/nonexistent/x', 'r');
echo error_get_last()['message'], "\n";
PHP);
[$out, $status] = rn_exec($d, "$d/main.php");
echo $out, "status: $status\n";
unlink("$d/m");
rn_clean($d);
?>
--EXPECT--
Warning: fopen(/nonexistent/x): Failed to open stream: No such file or directory in DIR/main.php on line 7
fopen(/nonexistent/x): Failed to open stream: No such file or directory
status: 0
