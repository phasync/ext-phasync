--TEST--
memo across run_next() requests: what one request decoded and read is a hit in a later one (stored the second time, handed out from the third); a file changed between requests is read again; evicted entries are freed when a request ends
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/data.json", json_encode(['name' => 'opt', 'list' => range(1, 50), 'o' => ['k' => 'v']]));
file_put_contents("$d/loop.php", <<<'PHP'
<?php
$n = (int) @file_get_contents($argv[1] . '.n') + 1;
file_put_contents($argv[1] . '.n', $n);
$s0 = phasync\ext\memo_stats();
$text = file_get_contents($argv[1]);
$a = json_decode($text, true);
$o = json_decode($text);
$u = unserialize(serialize($a));
$a['list'][] = 'mine';        // writes to hits separate: never seen by the next request
$o->o->k = 'mine';
$s = phasync\ext\memo_stats();
printf("run %d: %s %s %s, hits file %d json %d unserialize %d, graveyard at start %d\n", $n, $o->name, $o->o->k, count($u['list']),
    $s['file_hits'] - $s0['file_hits'], $s['json_hits'] - $s0['json_hits'], $s['unserialize_hits'] - $s0['unserialize_hits'], $s0['graveyard_bytes']);
if ($n === 3) {
    usleep(10000);
    file_put_contents($argv[1], json_encode(['name' => 'changed', 'list' => [1], 'o' => ['k' => 'v']]));
}
if ($n === 4) {
    $keep = [];
    for ($i = 0; $i < 2000; $i++) {   // churn: evictions into the graveyard, values still held
        $t = json_encode(['i' => $i, 'pad' => range(1, 30)]);
        json_decode($t, true); json_decode($t, true); $keep[] = json_decode($t, true);
    }
    $g = phasync\ext\memo_stats()['graveyard_bytes'];
    echo 'graveyard in use: ', $g > 0 ? 'yes' : 'no', ', values intact: ', var_export($keep[0]['i'] === 0 && $keep[1999]['pad'][29] === 30, true), "\n";
}
if ($n < 5) {
    phasync\ext\run_next(__FILE__);
}
PHP);
[$out, $status] = rn_exec($d, "$d/loop.php", ["$d/data.json"], null, ['ini' => ['phasync.memo_memory' => '256K']]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
run 1: opt mine 50, hits file 0 json 0 unserialize 0, graveyard at start 0
run 2: opt mine 50, hits file 0 json 0 unserialize 0, graveyard at start 0
run 3: opt mine 50, hits file 1 json 2 unserialize 1, graveyard at start 0
run 4: changed mine 1, hits file 0 json 0 unserialize 0, graveyard at start 0
graveyard in use: yes, values intact: true
run 5: changed mine 1, hits file 0 json 0 unserialize 0, graveyard at start 0
status: 0
