<?php
use function phasync\ext\set_preempt_function;

var_dump(opcache_get_status()["jit"]["on"]);

function spin(): int
{
    global $calls;
    $calls = 0;
    $end = microtime(true) + 0.1;
    $x = 0;
    while (microtime(true) < $end) {
        $x = ($x * 7 + 3) % 1001;
    }
    return $calls;
}

$calls = 0;
set_preempt_function(function () use (&$calls) { $calls++; }, 0.005);
echo "tracing: ", spin() >= 3 ? "interrupted" : "calls: $calls", "\n";
set_preempt_function(null);

ini_set('opcache.jit', 'off');
var_dump(opcache_get_status()['jit']['on']);
set_preempt_function(function () use (&$calls) { $calls++; }, 0.005);
echo "off: ", spin() >= 3 ? "interrupted" : "calls: $calls", "\n";
set_preempt_function(null);
?>
