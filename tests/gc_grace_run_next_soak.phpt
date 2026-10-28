--TEST--
phasync.gc_grace under run_next(): 200 runs that leave young cyclic garbage and live data keep the memory flat
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", '<?php
require ' . var_export(__DIR__ . '/gc_grace.inc', true) . ';
$state = __DIR__ . "/state";
$s = is_file($state) ? json_decode(file_get_contents($state), true) : ["n" => 0, "log" => []];
$s["n"]++;
$keep = [];
for ($i = 0; $i < 20; $i++) {
    gc_cycles(500, $keep);
    gc_cycles(500);
    gc_collect_cycles();
    if ($i % 5 === 4) { $keep = []; }
}
if ($s["n"] >= 50) {
    $s["log"][] = (int) preg_replace("/\D/", "", explode("VmRSS:", file_get_contents("/proc/self/status"))[1]);
}
file_put_contents($state, json_encode($s));
if ($s["n"] < 200) {
    phasync\ext\run_next(__FILE__);
    return;
}
$rss = $s["log"];
echo "runs: ", $s["n"], "\n";
echo "rss growth: ", (max($rss) - $rss[0]) < 2048 ? "flat" : (max($rss) - $rss[0]) . " kB", "\n";
');
[$out, $status] = rn_exec($d, "$d/main.php", [], null, ['ini' => ['phasync.gc_grace' => '60000']]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
runs: 200
rss growth: flat
status: 0
