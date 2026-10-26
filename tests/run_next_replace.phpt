--TEST--
run_next(): calling it again replaces the recording; a relative path is resolved at the call; it is one-shot
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
mkdir("$d/sub");
file_put_contents("$d/main.php", <<<'PHP'
<?php
phasync\ext\run_next(__DIR__ . '/a.php');
phasync\ext\run_next('sub/b.php');   // replaces a.php; relative to the cwd now
chdir(__DIR__ . '/sub');
echo "main\n";
PHP);
file_put_contents("$d/a.php", "<?php echo \"a\\n\";");
file_put_contents("$d/sub/b.php", "<?php echo \"b, cwd=\", basename(getcwd()), \"\\n\";");
[$out, $status] = rn_exec($d, "$d/main.php");
echo $out, "status: $status\n";
unlink("$d/sub/b.php");
rmdir("$d/sub");
rn_clean($d);
?>
--EXPECT--
main
b, cwd=sub
status: 0
