--TEST--
phasync.gc_grace=0 (the default): collections hide nothing
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . "/gc_grace.inc";
var_dump(ini_get("phasync.gc_grace"));
gc_cycles(1000);
gc_line("young", gc_collect_cycles());
echo json_encode(phasync\ext\gc_grace_stats()), "\n";
?>
--EXPECT--
string(1) "0"
young: freed 2000, gone 2000, held 0
{"grace_ms":0,"collections":0,"hidden":0,"held":0,"rerooted":0,"scan_ns":0}
