--TEST--
phasync.gc_grace: destructors run by a collection that create cycles, free others and collect again
--EXTENSIONS--
phasync
--INI--
phasync.gc_grace=100
--FILE--
<?php
require __DIR__ . "/gc_grace.inc";
class Busy { public $self; public static $n = 0; function __destruct() {
    if (self::$n++ < 50) { gc_cycles(20); gc_collect_cycles(); $x = new Busy; $x->self = $x; }
} }
for ($i = 0; $i < 100; $i++) { $b = new Busy; $b->self = $b; }
$b = null;
gc_collect_cycles();
gc_cycles(100);
for ($r = 0; $r < 6; $r++) { gc_collect_cycles(); usleep(150000); }
gc_collect_cycles();
$s = phasync\ext\gc_grace_stats();
echo "busy destructed: ", Busy::$n, ", nodes gone: ", GcNode::$gone, ", held: $s[held]\n";
?>
--EXPECT--
busy destructed: 150, nodes gone: 2200, held: 0
