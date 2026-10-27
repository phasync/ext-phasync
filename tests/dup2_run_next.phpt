--TEST--
dup2(): the descriptor survives into the run_next() request that follows
--EXTENSIONS--
phasync
pcntl
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", <<<'PHP'
<?php
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
$pid = pcntl_fork();
if ($pid === 0) {
    fclose($a);
    phasync\ext\dup2($b, 7);
    fclose($b);
    phasync\ext\run_next(__DIR__ . '/second.php');
    exit(0);
}
fclose($b);
pcntl_waitpid($pid, $status);
echo fread($a, 20);
PHP);
file_put_contents("$d/second.php", <<<'PHP'
<?php
$out = fopen('php://fd/7', 'w');
fwrite($out, "from second\n");
fclose($out);
PHP);
[$out, $status] = rn_exec($d, "$d/main.php", [], null, ['ini' => ['disable_functions' => '']]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
from second
status: 0
