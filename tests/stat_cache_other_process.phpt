--TEST--
stat cache: a change by another process is seen after a short bounded wait (the watcher thread), with no signal interrupting the parent's blocking calls
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Linux') die('skip Linux only'); ?>
--INI--
phasync.stat_cache=1
--FILE--
<?php
$d = sys_get_temp_dir() . '/phasync-sc-other-' . getmypid();
mkdir($d);
$f = "$d/f";
var_dump(file_exists($f), file_exists($f));
$p = proc_open([PHP_BINARY, '-n', '-r', 'file_put_contents($argv[1], "abc"); echo "done\n"; sleep(1);', $f],
    [1 => ['pipe', 'w']], $pipes);
// the parent waits in select() while the child writes /tmp: no EINTR
$r = [$pipes[1]]; $w = $e = null;
var_dump(stream_select($r, $w, $e, 5), trim(fgets($pipes[1])));
$t = hrtime(true);
while (!file_exists($f) && hrtime(true) - $t < 1e9) {
    usleep(100);
}
var_dump(file_exists($f), filesize($f));
// a stat of a stable file between waits is a hit
var_dump(stream_select($r, $w, $e, 0, 200000));
fclose($pipes[1]);
proc_close($p);
unlink($f);
rmdir($d);
?>
--EXPECT--
bool(false)
bool(false)
int(1)
string(4) "done"
bool(true)
int(3)
int(0)
