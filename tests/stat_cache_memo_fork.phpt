--TEST--
stat cache: memo listings keyed by the directory's cached stat see an entry another process adds at once; a fork()ed child starts with an empty cache of its own and sees changes too
--EXTENSIONS--
phasync
pcntl
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
phasync.memo_memory=8M
--FILE--
<?php
$d = sys_get_temp_dir() . '/phasync-sc-memo-' . getmypid();
mkdir($d);
touch("$d/a");
for ($i = 0; $i < 3; $i++) { $l = scandir($d); }
echo implode(',', $l), ' dir_hits ', phasync\ext\memo_stats()['dir_hits'], "\n";
exec('touch ' . escapeshellarg("$d/b"));     // the same second: only the cache's event tells
echo implode(',', scandir($d)), "\n";
var_dump(file_exists("$d/c"));
$pid = pcntl_fork();
if ($pid === 0) {
    $s = phasync\ext\stat_cache_stats();
    echo "child: watches {$s['watches']} entries {$s['entries']}\n";
    var_dump(file_exists("$d/c"), file_exists("$d/c"));
    exec('touch ' . escapeshellarg("$d/c"));
    var_dump(file_exists("$d/c"));
    exit(0);
}
pcntl_waitpid($pid, $status);
var_dump(file_exists("$d/c"));
exec('rm -rf ' . escapeshellarg($d));
?>
--EXPECT--
.,..,a dir_hits 1
.,..,a,b
bool(false)
child: watches 0 entries 0
bool(false)
bool(false)
bool(true)
bool(true)
