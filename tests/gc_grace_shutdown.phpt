--TEST--
phasync.gc_grace: in the request's shutdown, a collection takes all garbage regardless of age, and every hidden object is destructed
--EXTENSIONS--
phasync
--INI--
phasync.gc_grace=60000
--FILE--
<?php
require __DIR__ . "/gc_grace.inc";
gc_cycles(1000);
gc_line("young", gc_collect_cycles());
register_shutdown_function(function () {
    gc_line("shutdown", gc_collect_cycles());
    gc_cycles(5);
});
class Last { function __destruct() { echo "at the end: gone ", GcNode::$gone, "\n"; } }
$last = new Last;
?>
--EXPECT--
young: freed 0, gone 0, held 2000
shutdown: freed 2000, gone 2000, held 0
at the end: gone 2000
