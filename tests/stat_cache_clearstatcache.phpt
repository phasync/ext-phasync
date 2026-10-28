--TEST--
stat cache: results around clearstatcache() are PHP's own (its one-entry cache answers first, as without us); clearstatcache($clear, $filename) drops that path's result, under any spelling
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
--FILE--
<?php
$d = sys_get_temp_dir() . '/phasync-sc-clear-' . getmypid();
mkdir($d);
$f = "$d/f";
touch($f);
var_dump(is_file($f), filesize($f));
exec('printf abc > ' . escapeshellarg($f));
var_dump(filesize($f));
clearstatcache();
var_dump(filesize($f));
exec('rm ' . escapeshellarg($f));
var_dump(is_file($f), file_exists($f));   // is_file: PHP's cache; file_exists never cached by PHP
clearstatcache(true, $f);
var_dump(is_file($f));
touch($f);
is_dir($d);
var_dump(is_file($f), is_file("$d/../" . basename($d) . '/f'));
$e = phasync\ext\stat_cache_stats()['entries'];
clearstatcache(true, "$d/../" . basename($d) . '/f');   // another spelling: its result goes
var_dump(phasync\ext\stat_cache_stats()['entries'] === $e - 1, is_file($f));
unlink($f);
rmdir($d);
?>
--EXPECT--
bool(true)
int(0)
int(3)
int(3)
bool(false)
bool(false)
bool(false)
bool(true)
bool(true)
bool(true)
bool(true)
