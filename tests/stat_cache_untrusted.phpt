--TEST--
stat cache: on a filesystem inotify cannot vouch for (forced here by the test hook phasync.test_stat_cache_untrusted) nothing is cached or watched, and every result is PHP's own
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
phasync.test_stat_cache_untrusted=1
--FILE--
<?php
$d = sys_get_temp_dir() . '/phasync-sc-untrusted-' . getmypid();
mkdir($d);
$f = "$d/f";
$show = static function () use ($f) {
    for ($i = 0; $i < 2; $i++) {
        is_dir('/');
        $r = [file_exists($f), is_file($f), is_readable($f), @filesize($f), realpath($f) !== false];   // realpath(): PHP's realpath cache
    }
    echo json_encode($r), "\n";
};
$show();
exec('printf abc > ' . escapeshellarg($f));
$show();
exec('rm ' . escapeshellarg($f));
$show();
rmdir($d);
$s = phasync\ext\stat_cache_stats();
var_dump($s['enabled'], $s['hits'], $s['watches'], $s['entries'], $s['fallbacks'] > 0);
?>
--EXPECT--
[false,false,false,false,false]
[true,true,true,3,true]
[false,false,false,false,true]
bool(true)
int(0)
int(0)
int(0)
bool(true)
