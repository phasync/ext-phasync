--TEST--
run_next(): a fork()ed child that ends does not run the next lifecycle; a child that records its own does
--EXTENSIONS--
phasync
pcntl
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", <<<'PHP'
<?php
$state = __DIR__ . '/state';
$n = (int) @file_get_contents($state) + 1;
file_put_contents($state, $n);
echo "parent lifecycle $n\n";
if ($n === 1) {
    phasync\ext\run_next(__FILE__);
    $pid = pcntl_fork();
    if ($pid === 0) {
        echo "child 1 ends\n";
        exit(0);
    }
    pcntl_waitpid($pid, $status);
    $pid = pcntl_fork();
    if ($pid === 0) {
        phasync\ext\run_next(__DIR__ . '/child.php');
        echo "child 2 ends\n";
        exit(0);
    }
    pcntl_waitpid($pid, $status);
}
PHP);
file_put_contents("$d/child.php", '<?php echo "child 2 reruns\n";');
[$out, $status] = rn_exec($d, "$d/main.php", [], null, ['ini' => ['disable_functions' => '']]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
parent lifecycle 1
child 1 ends
child 2 ends
child 2 reruns
parent lifecycle 2
status: 0
