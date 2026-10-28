--TEST--
stat cache: an own write is seen by the very next stat, without waiting for the watcher thread (1000 rounds)
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
--FILE--
<?php
$d = sys_get_temp_dir() . '/phasync-sc-loop-' . getmypid();
mkdir($d);
$f = "$d/f";
$bad = 0;
for ($i = 1; $i <= 1000; $i++) {
    file_put_contents($f, str_repeat('x', $i));
    $bad += filesize($f) !== $i;
    $h = fopen($f, 'a'); fwrite($h, 'y'); fflush($h);
    $bad += filesize($f) !== $i + 1;
    fclose($h);
    unlink($f);
    $bad += file_exists($f);
    touch($f);
    $bad += !is_file($f);
    rename($f, "$f.2");
    $bad += file_exists($f) || !file_exists("$f.2");
    unlink("$f.2");
}
var_dump($bad, phasync\ext\stat_cache_stats()['enabled']);
rmdir($d);
?>
--EXPECT--
int(0)
bool(true)
