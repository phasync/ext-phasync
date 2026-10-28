<?php
// phasync.gc_grace tests: N objects in two-node cycles, counted by their destructors
class GcNode { public static int $gone = 0; public $o; public $pad = [1, 2, 3]; function __destruct() { self::$gone++; } }
function gc_cycles(int $k, ?array &$keep = null): void
{
    for ($i = 0; $i < $k; $i++) {
        $a = new GcNode; $b = new GcNode; $a->o = $b; $b->o = $a;
        if ($keep !== null) { $keep[] = $a; }
    }
}
function gc_line(string $what, int $freed): void
{
    $s = phasync\ext\gc_grace_stats();
    echo "$what: freed $freed, gone ", GcNode::$gone, ", held $s[held]\n";
}
