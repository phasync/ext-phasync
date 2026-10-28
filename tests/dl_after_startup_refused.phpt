--TEST--
Loading phasync with dl() after startup fails loudly instead of crashing (issue #9, FPM php_admin_value[extension])
--SKIPIF--
<?php if (!function_exists('proc_open')) die('skip proc_open'); ?>
--FILE--
<?php
$cmd = [PHP_BINARY, '-n', '-d', 'enable_dl=1', '-d', 'display_errors=1', '-d', 'extension_dir=' . ini_get('extension_dir'),
	'-r', 'dl("phasync.so"); echo "not reached\n";'];
$p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
$out = stream_get_contents($pipes[1]) . stream_get_contents($pipes[2]);
echo trim(strtok($out, "\n")), "\n", 'exit ', proc_close($p), "\n";
?>
--EXPECT--
Fatal error: phasync cannot be loaded after startup (dl() or php_admin_value[extension]); load it with extension=phasync.so in php.ini, or with -d extension= on the php/php-fpm command line in Unknown on line 0
exit 255
