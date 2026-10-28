--TEST--
stat cache: memo listings keyed by the directory's cached stat see an entry another process adds at once; a fork()ed child starts with an empty cache of its own and sees changes too, while keeping the memo entries it inherits for what is unchanged
--EXTENSIONS--
phasync
pcntl
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.memo_min_ns=0
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
file_put_contents("$d/a", str_repeat('x', 100));
scandir($d); scandir($d); file_get_contents("$d/a"); file_get_contents("$d/a");
$pid = pcntl_fork();
if ($pid === 0) {
    $s = phasync\ext\stat_cache_stats();
    echo "child: watches {$s['watches']} entries {$s['entries']}\n";
    var_dump(file_exists("$d/c"), file_exists("$d/c"));
    $m = phasync\ext\memo_stats();
    $l = scandir($d); $f = file_get_contents("$d/a");
    $n = phasync\ext\memo_stats();
    echo 'child: ', implode(',', $l), ' dir hit ', $n['dir_hits'] - $m['dir_hits'], ', file hit ', $n['file_hits'] - $m['file_hits'], ' ', strlen($f), "\n";
    exec('touch ' . escapeshellarg("$d/c"));
    var_dump(file_exists("$d/c"));
    echo implode(',', scandir($d)), "\n";
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
child: .,..,a,b dir hit 1, file hit 1 100
bool(true)
.,..,a,b,c
bool(true)
