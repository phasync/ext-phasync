--TEST--
memo scandir() and glob(): a listing made before (seen twice) comes back as the cached array, keyed by the listed directory's own stat (which an added, removed or renamed entry changes) and the arguments; a glob with wildcards before the last slash is left alone
--EXTENSIONS--
phasync
--INI--
phasync.memo_min_ns=0
phasync.memo_memory=64M
--FILE--
<?php
$dir = sys_get_temp_dir() . '/phasync-memo-dir-' . getmypid();
@mkdir($dir);
foreach (['a.json', 'b.json', 'c.txt'] as $f) { touch("$dir/$f"); }
@mkdir("$dir/sub");
touch("$dir/sub/d.json");
$hits = static fn () => phasync\ext\memo_stats()['dir_hits'];

$h = $hits();
for ($i = 0; $i < 3; $i++) { $list = scandir($dir); }
echo 'scandir: ', implode(',', $list), ', hits ', $hits() - $h, "\n";
$h = $hits();
for ($i = 0; $i < 3; $i++) { $desc = scandir($dir, SCANDIR_SORT_DESCENDING); }
echo 'scandir descending: ', implode(',', $desc), ', hits ', $hits() - $h, "\n";
$h = $hits();
for ($i = 0; $i < 3; $i++) { $g = glob("$dir/*.json"); }
echo 'glob *.json: ', implode(',', array_map('basename', $g)), ', hits ', $hits() - $h, "\n";

usleep(10000);
touch("$dir/e.json");                       // a new entry: the directory changed
echo 'after adding e.json: ', implode(',', array_map('basename', glob("$dir/*.json"))), ' / ', count(scandir($dir)), " entries\n";
unlink("$dir/a.json");
echo 'after removing a.json: ', implode(',', array_map('basename', glob("$dir/*.json"))), "\n";

$h = $hits();
for ($i = 0; $i < 3; $i++) { $deep = glob("$dir/*/*.json"); }
echo 'glob */*.json: ', implode(',', array_map('basename', $deep)), ', hits ', $hits() - $h, "\n";

foreach (glob("$dir/*") as $f) { is_dir($f) ? array_map('unlink', glob("$f/*")) && rmdir($f) : unlink($f); }
rmdir($dir);
?>
--EXPECT--
scandir: .,..,a.json,b.json,c.txt,sub, hits 1
scandir descending: sub,c.txt,b.json,a.json,..,., hits 1
glob *.json: a.json,b.json, hits 1
after adding e.json: a.json,b.json,e.json / 7 entries
after removing a.json: b.json,e.json
glob */*.json: d.json, hits 0
