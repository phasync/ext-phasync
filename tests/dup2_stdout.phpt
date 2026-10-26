--TEST--
dup2() onto fd 1 redirects later output to the target descriptor
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!function_exists('proc_open')) die('skip requires proc_open'); ?>
--FILE--
<?php
$log = tempnam(sys_get_temp_dir(), 'phasync_dup2_');
$script = tempnam(sys_get_temp_dir(), 'phasync_dup2_') . '.php';
file_put_contents($script, <<<PHP
<?php
\$out = fopen('$log', 'w');
phasync\\ext\\dup2(\$out, 1);
fclose(\$out);
echo "redirected\\n";
PHP);

$ext = 'extension=' . realpath(ini_get('extension_dir') . '/phasync.so');
$p = proc_open([PHP_BINARY, '-n', '-d', $ext, $script], [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
$stdout = stream_get_contents($pipes[1]);
$stderr = stream_get_contents($pipes[2]);
proc_close($p);

var_dump($stdout);
var_dump($stderr);
echo file_get_contents($log);

unlink($log);
unlink($script);
?>
--EXPECT--
string(0) ""
string(0) ""
redirected
