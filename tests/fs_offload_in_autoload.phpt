--TEST--
phasync.fs_offload=all: a filesystem call inside an autoload in the worker's own context does not suspend, so the class is never half-loaded while other code (the scheduler) needs it (issue #66)
--EXTENSIONS--
phasync
--INI--
phasync.fs_offload=all
--FILE--
<?php
require __DIR__ . '/loop.inc';
$dir = sys_get_temp_dir() . '/phasync-al-' . getmypid();
mkdir($dir);
file_put_contents("$dir/AlK.php", '<?php class AlK { const V = "k"; }');
spl_autoload_register(function ($c) use ($dir) {
    if (is_file("$dir/$c.php")) require "$dir/$c.php";
});
$loop = new Loop;
$loop->runAll(
    function () { echo 'a: ', AlK::V, "\n"; },
    function () { echo 'b: ', class_exists('AlK') ? 'loaded' : 'NOT FOUND', "\n"; },
);
unlink("$dir/AlK.php"); rmdir($dir);
?>
--EXPECT--
a: k
b: loaded
