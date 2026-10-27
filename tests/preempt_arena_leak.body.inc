<?php

class BindScope {}

// The template closure is never itself bound to a scope. Every
// Closure::bind() to a scope therefore takes the engine's non-shared
// (heap) run-time-cache path and hands back a closure with a fresh,
// zeroed run-time cache -- so phasync's own per-op_array cached info,
// stored in that run-time cache, must be rebuilt. Before the fix, that
// rebuild allocates permanently on CG(arena) (a bump allocator that only
// grows in ~64 KiB chunks, so memory_get_usage() is blind to individual
// allocations but shows a clean jump whenever a chunk fills up).
$template = function () {
    $t = microtime(true);
    while (microtime(true) - $t < 0.0003);
};

preempt_handler(function () {}, 0.0002);

function run_round(int $n, Closure $template): void
{
    for ($i = 0; $i < $n; $i++) {
        $bound = Closure::bind($template, null, BindScope::class);
        $bound();
        unset($bound);
    }
}

run_round(500, $template);   // warm-up: one-time engine/extension costs
gc_collect_cycles();
$before = memory_get_usage();

run_round(9000, $template);  // over 64 KiB worth of leaked allocations, if leaking
gc_collect_cycles();
$after = memory_get_usage();

preempt_handler(null);

$growth = $after - $before;
// Without the bug: bind()/call()/unset() bookkeeping only, at most a few
// hundred bytes. With it: one whole arena chunk (64 KiB) leaks here.
echo $growth < 32768 ? "stable\n" : "growing: $growth bytes over 9000 closures\n";
?>
