--TEST--
stat cache: a change made by another process (create, write, chmod, rename, delete of the file, or rename/delete of a directory above it) is seen by the very next call; negative results flip when the file appears
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
--FILE--
<?php
umask(022);
$root = sys_get_temp_dir() . '/phasync-sc-ext-' . getmypid();
$d = "$root/a/b";
$f = "$d/f.txt";
$sh = static fn (string $cmd) => exec($cmd);
$q = 'escapeshellarg';
$show = static function (string $what) use ($f) {
    for ($i = 0; $i < 3; $i++) {   // repeated: the second and third are hits
        $r = [file_exists($f), is_file($f), is_dir(dirname($f)), is_readable($f), is_writable($f),
              @filesize($f), @fileperms($f) & 0777, file_exists(dirname($f) . '/../b/f.txt')];
    }
    echo str_pad($what, 22), json_encode($r), "\n";
};
$sh('mkdir -p ' . $q($d));
$show('missing');
$sh('printf abc > ' . $q($f));
$show('created');
$sh('printf defgh >> ' . $q($f));
$show('appended');
$sh('chmod 0400 ' . $q($f));
$show('chmod 0400');
$sh('mv ' . $q($f) . ' ' . $q("$d/g.txt"));
$show('renamed away');
$sh('mv ' . $q("$d/g.txt") . ' ' . $q($f));
$show('renamed back');
$sh('rm ' . $q($f));
$show('deleted');
$sh('printf x > ' . $q($f));
$show('created again');
$sh('mv ' . $q("$root/a") . ' ' . $q("$root/z"));
$show('ancestor renamed');
$sh('mv ' . $q("$root/z") . ' ' . $q("$root/a"));
$show('ancestor back');
$sh('rm -r ' . $q("$root/a"));
$show('ancestor deleted');
$sh('mkdir -p ' . $q($d) . ' && printf yz > ' . $q($f));
$show('all recreated');

// a missing directory deeper down: the negative result flips when it appears
$deep = "$root/a/new/deeper/x.php";
var_dump(file_exists($deep), file_exists($deep));
$sh('mkdir -p ' . $q(dirname($deep)) . ' && touch ' . $q($deep));
var_dump(file_exists($deep));

// filemtime follows a touch -d
$sh('touch -d @1000000000 ' . $q($f));
var_dump(filemtime($f));
$sh('touch -d @1100000000 ' . $q($f));
var_dump(filemtime($f));

// a listing keyed by the directory's stat (memo) is not involved here; scandir sees new entries
$sh('touch ' . $q("$d/h.txt"));
echo implode(',', scandir($d)), "\n";

$s = phasync\ext\stat_cache_stats();
var_dump($s['enabled'], $s['hits'] > 50, $s['misses'] > 0, $s['flushes'] > 0);
$sh('rm -rf ' . $q($root));
?>
--EXPECT--
missing               [false,false,true,false,false,false,0,false]
created               [true,true,true,true,true,3,420,true]
appended              [true,true,true,true,true,8,420,true]
chmod 0400            [true,true,true,true,false,8,256,true]
renamed away          [false,false,true,false,false,false,0,false]
renamed back          [true,true,true,true,false,8,256,true]
deleted               [false,false,true,false,false,false,0,false]
created again         [true,true,true,true,true,1,420,true]
ancestor renamed      [false,false,false,false,false,false,0,false]
ancestor back         [true,true,true,true,true,1,420,true]
ancestor deleted      [false,false,false,false,false,false,0,false]
all recreated         [true,true,true,true,true,2,420,true]
bool(false)
bool(false)
bool(true)
int(1000000000)
int(1100000000)
.,..,f.txt,h.txt
bool(true)
bool(true)
bool(true)
bool(true)
