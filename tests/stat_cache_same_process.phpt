--TEST--
stat cache: this process's own writes through PHP (fopen w/a/x/c, file_put_contents, fwrite, ftruncate, unlink, rename, mkdir, rmdir, touch, chmod, copy, tempnam, symlink) are seen at once, exactly as without the cache
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
--FILE--
<?php
umask(022);
$d = sys_get_temp_dir() . '/phasync-sc-self-' . getmypid();
mkdir($d);
$f = "$d/f.txt";
$other = "$d/o.txt";
$show = static function (string $what, string $p) use ($other) {
    for ($i = 0; $i < 2; $i++) {
        is_file($other);   // PHP's one-entry stat cache holds another path, so ours answers
        $r = [file_exists($p), is_file($p), is_dir($p), is_readable($p), is_writable($p), is_link($p), is_dir($p) ? "d" : @filesize($p), @fileperms($p) & 0777, @filetype($p)];
    }
    echo str_pad($what, 24), json_encode($r), "\n";
};
touch($other);
$show('nothing', $f);
$h = fopen($f, 'x'); $show('fopen x', $f); fwrite($h, 'abc'); fflush($h); $show('fwrite', $f); fclose($h);
$h = fopen($f, 'a'); fwrite($h, 'de'); fclose($h); $show('fopen a', $f);
$h = fopen($f, 'w'); $show('fopen w', $f); fclose($h);
$h = fopen($f, 'c+'); fwrite($h, '123456'); ftruncate($h, 4); fclose($h); $show('fopen c+, ftruncate', $f);
file_put_contents($f, 'hello world'); $show('file_put_contents', $f);
chmod($f, 0600); $show('chmod 0600', $f);
touch($f, 1000000000); var_dump(filemtime($f)); touch($f, 1100000000); is_file($other); var_dump(filemtime($f));
rename($f, "$f.2"); $show('renamed away', $f); $show('renamed to', "$f.2");
copy("$f.2", $f); $show('copy', $f);
unlink("$f.2"); $show('unlinked', "$f.2");
symlink($f, "$d/link"); $show('symlink', "$d/link");
unlink($f); $show('link target gone', "$d/link");
unlink("$d/link");
mkdir("$d/sub"); $show('mkdir', "$d/sub");
$show('in new dir', "$d/sub/x");
touch("$d/sub/x"); $show('touched', "$d/sub/x");
unlink("$d/sub/x"); rmdir("$d/sub"); $show('rmdir', "$d/sub"); $show('under rmdir', "$d/sub/x");
$t = tempnam($d, 'tmp'); $show('tempnam', $t); unlink($t);
unlink($other); rmdir($d);
echo phasync\ext\stat_cache_stats()['hits'] > 20 ? "cached\n" : "not cached\n";
?>
--EXPECT--
nothing                 [false,false,false,false,false,false,false,0,false]
fopen x                 [true,true,false,true,true,false,0,420,"file"]
fwrite                  [true,true,false,true,true,false,3,420,"file"]
fopen a                 [true,true,false,true,true,false,5,420,"file"]
fopen w                 [true,true,false,true,true,false,0,420,"file"]
fopen c+, ftruncate     [true,true,false,true,true,false,4,420,"file"]
file_put_contents       [true,true,false,true,true,false,11,420,"file"]
chmod 0600              [true,true,false,true,true,false,11,384,"file"]
int(1000000000)
int(1100000000)
renamed away            [false,false,false,false,false,false,false,0,false]
renamed to              [true,true,false,true,true,false,11,384,"file"]
copy                    [true,true,false,true,true,false,11,420,"file"]
unlinked                [false,false,false,false,false,false,false,0,false]
symlink                 [true,true,false,true,true,true,11,420,"link"]
link target gone        [false,false,false,false,false,true,false,0,"link"]
mkdir                   [true,false,true,true,true,false,"d",493,"dir"]
in new dir              [false,false,false,false,false,false,false,0,false]
touched                 [true,true,false,true,true,false,0,420,"file"]
rmdir                   [false,false,false,false,false,false,false,0,false]
under rmdir             [false,false,false,false,false,false,false,0,false]
tempnam                 [true,true,false,true,true,false,0,384,"file"]
cached
