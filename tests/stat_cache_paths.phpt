--TEST--
stat cache: symlinked directories and symlinks as the last component, "..", "//" and "." spellings, relative paths, file:// and a user wrapper registered as file:// all give PHP's own results as other processes change things
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
--FILE--
<?php
umask(022);
$d = sys_get_temp_dir() . '/phasync-sc-paths-' . getmypid();
$q = 'escapeshellarg';
exec('mkdir -p ' . $q("$d/real/sub") . ' && ln -s real ' . $q("$d/ln") . ' && ln -s real/x ' . $q("$d/lnf"));
chdir($d);
$paths = ["$d/real/x", "$d/ln/x", "$d/lnf", "$d/real/sub/../x", "$d//real/./x", "$d/real/nope/../x", "real/x", "file://$d/real/x", "$d/real/x/"];
$show = static function (string $what) use ($paths) {
    $r = [];
    foreach ($paths as $p) {
        for ($i = 0; $i < 2; $i++) {
            is_dir('/');
            $v = [file_exists($p), is_file($p), is_link($p), @filesize($p), realpath($p) !== false];
        }
        $r[] = implode('', array_map(static fn ($b) => is_bool($b) ? (int) $b : "($b)", $v));
    }
    echo str_pad($what, 12), implode(' ', $r), "\n";
};
$show('none');
exec('printf abc > ' . $q("$d/real/x"));
$show('created');
exec('printf abcdef > ' . $q("$d/real/x"));
$show('rewritten');
exec('rm ' . $q("$d/ln") . ' && mkdir ' . $q("$d/ln") . ' && printf z > ' . $q("$d/ln/x"));
$show('ln is a dir');
exec('rm ' . $q("$d/real/x"));
$show('deleted');

class W {
    public $context;
    public function url_stat(string $p, int $f) { echo "W::url_stat\n"; return false; }
}
stream_wrapper_unregister('file');
stream_wrapper_register('file', 'W');
var_dump(is_file("$d/real/sub"));
stream_wrapper_restore('file');
var_dump(is_dir("$d/real/sub"));
exec('rm -rf ' . $q($d));
?>
--EXPECT--
none        00000 00000 00100 00000 00000 00000 00000 00000 00000
created     110(3)1 110(3)1 111(3)1 110(3)1 110(3)1 00000 110(3)1 110(3)0 00000
rewritten   110(6)1 110(6)1 111(6)1 110(6)1 110(6)1 00000 110(6)1 110(6)0 00000
ln is a dir 110(6)1 110(1)1 111(6)1 110(6)1 110(6)1 00000 110(6)1 110(6)0 00000
deleted     00001 110(1)1 00101 00001 00001 00000 00001 00000 00000
W::url_stat
bool(false)
bool(true)
