--TEST--
phasync.gc_grace under the tracing JIT: young cyclic garbage is hidden, and collected once old
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!extension_loaded("Zend OPcache")) die("skip needs opcache"); ?>
--INI--
phasync.gc_grace=1000
opcache.enable=1
opcache.enable_cli=1
opcache.jit=tracing
opcache.jit_buffer_size=64M
opcache.jit_hot_loop=1
opcache.jit_hot_func=1
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
