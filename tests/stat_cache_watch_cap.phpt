--TEST--
stat cache: paths in more directories than phasync.stat_cache_watches allows stay correct; reaching the cap drops everything and starts over
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
phasync.stat_cache_watches=16
--FILE--
<?php
$d = sys_get_temp_dir() . '/phasync-sc-cap-' . getmypid();
mkdir($d);
for ($i = 0; $i < 40; $i++) {
    mkdir("$d/$i");
    for ($j = 0; $j < 5; $j++) { touch("$d/$i/$j"); }
}
$check = static function (bool $gone) use ($d) {
    $bad = 0;
    for ($i = 0; $i < 40; $i++) {
        for ($round = 0; $round < 3; $round++) {
            for ($j = 0; $j < 7; $j++) {
                $bad += file_exists("$d/$i/$j") !== ($j < 5 && !($gone && $i === 7 && $j === 3));
            }
        }
    }
    return $bad;
};
var_dump($check(false));
exec('rm ' . escapeshellarg("$d/7/3"));
var_dump($check(true));
$s = phasync\ext\stat_cache_stats();
var_dump($s['flushes'] > 0, $s['watches'] <= 16 + 8, $s['hits'] > 0);
exec('rm -rf ' . escapeshellarg($d));
?>
--EXPECT--
int(0)
int(0)
bool(true)
bool(true)
bool(true)
