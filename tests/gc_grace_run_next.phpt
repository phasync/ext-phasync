--TEST--
phasync.gc_grace under run_next(): every run starts with no ages and nothing hidden, and young garbage left by one run is not the next one's
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
$n = is_file(__DIR__ . "/n") ? (int) file_get_contents(__DIR__ . "/n") : 0;
file_put_contents(__DIR__ . "/n", ++$n);
echo "run $n: ", json_encode(phasync\ext\gc_grace_stats()), "\n";
if ($n === 1) {
    $keep = [];
    gc_cycles(500, $keep);           // the handles the next run reuses
    gc_collect_cycles();
    usleep(300000);                  // old here
    $keep = null;
    gc_line("run 1 old", gc_collect_cycles());
    gc_cycles(500);
    gc_line("run 1 young, left", gc_collect_cycles());
    phasync\ext\run_next(__FILE__);
} else {
    gc_cycles(500);                  // same handles: young again
    gc_line("run 2 young", gc_collect_cycles());
    usleep(300000);
    gc_line("run 2 old", gc_collect_cycles());
}
');
[$out, $status] = rn_exec($d, "$d/main.php", [], null, ['ini' => ['phasync.gc_grace' => '200']]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
run 1: {"grace_ms":200,"collections":0,"hidden":0,"held":0,"rerooted":0,"scan_ns":0}
run 1 old: freed 1000, gone 1000, held 0
run 1 young, left: freed 0, gone 1000, held 1000
run 2: {"grace_ms":200,"collections":0,"hidden":0,"held":0,"rerooted":0,"scan_ns":0}
run 2 young: freed 0, gone 0, held 1000
run 2 old: freed 1000, gone 1000, held 0
status: 0
