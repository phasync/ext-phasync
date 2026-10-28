--TEST--
phasync.gc_grace: cyclic garbage younger than the grace survives gc_collect_cycles(), and is collected once old
--EXTENSIONS--
phasync
--INI--
phasync.gc_grace=1000
--FILE--
<?php
require __DIR__ . "/gc_grace.inc";
gc_cycles(1000);
gc_line("young", gc_collect_cycles());
gc_line("young again", gc_collect_cycles());
usleep(1100000);
gc_line("old", gc_collect_cycles());
$s = phasync\ext\gc_grace_stats();
echo "stats: grace $s[grace_ms], collections $s[collections], hidden $s[hidden], rerooted $s[rerooted]; gc_status collected ", gc_status()["collected"], "\n";
?>
--EXPECT--
young: freed 0, gone 0, held 2000
young again: freed 0, gone 0, held 2000
old: freed 2000, gone 2000, held 0
stats: grace 1000, collections 3, hidden 2000, rerooted 2000; gc_status collected 2000
