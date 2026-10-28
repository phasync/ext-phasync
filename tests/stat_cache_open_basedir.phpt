--TEST--
stat cache: open_basedir warnings and results are PHP's own, inside and outside the allowed tree, before and after a path was cached
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
--FILE--
<?php
$d = sys_get_temp_dir() . '/phasync-sc-obd-' . getmypid();
@mkdir("$d/in", 0777, true);
@mkdir("$d/out");
touch("$d/in/f");
touch("$d/out/f");
$all = static function () use ($d) {
    foreach (["$d/in/f", "$d/in/nope", "$d/out/f", "$d/in/../out/f"] as $p) {
        is_dir("$d/in");
        $r = [file_exists($p), is_file($p), is_readable($p), filemtime($p) > 0, realpath($p) !== false];
        echo basename(dirname($p)), '/', basename($p), ': ', json_encode($r), "\n";
    }
};
$all();                                   // cached before open_basedir
ini_set('open_basedir', "$d/in");
$all();
echo phasync\ext\stat_cache_stats()['hits'] > 0 ? "cached\n" : "not cached\n";
exec('rm -rf ' . escapeshellarg($d));
?>
--EXPECTF--
in/f: [true,true,true,true,true]

Warning: filemtime(): stat failed for %s/in/nope in %s on line 10
in/nope: [false,false,false,false,false]
out/f: [true,true,true,true,true]
out/f: [true,true,true,true,true]
in/f: [true,true,true,true,true]

Warning: filemtime(): stat failed for %s/in/nope in %s on line 10
in/nope: [false,false,false,false,false]

Warning: file_exists(): open_basedir restriction in effect. File(%s/out/f) is not within the allowed path(s): (%s/in) in %s on line 10

Warning: is_file(): open_basedir restriction in effect. File(%s/out/f) is not within the allowed path(s): (%s/in) in %s on line 10

Warning: is_readable(): open_basedir restriction in effect. File(%s/out/f) is not within the allowed path(s): (%s/in) in %s on line 10

Warning: filemtime(): open_basedir restriction in effect. File(%s/out/f) is not within the allowed path(s): (%s/in) in %s on line 10

Warning: realpath(): open_basedir restriction in effect. File(%s/out/f) is not within the allowed path(s): (%s/in) in %s on line 10
out/f: [false,false,false,false,false]

Warning: file_exists(): open_basedir restriction in effect. File(%s/in/../out/f) is not within the allowed path(s): (%s/in) in %s on line 10

Warning: is_file(): open_basedir restriction in effect. File(%s/in/../out/f) is not within the allowed path(s): (%s/in) in %s on line 10

Warning: is_readable(): open_basedir restriction in effect. File(%s/in/../out/f) is not within the allowed path(s): (%s/in) in %s on line 10

Warning: filemtime(): open_basedir restriction in effect. File(%s/in/../out/f) is not within the allowed path(s): (%s/in) in %s on line 10

Warning: realpath(): open_basedir restriction in effect. File(%s/out/f) is not within the allowed path(s): (%s/in) in %s on line 10
out/f: [false,false,false,false,false]
cached
