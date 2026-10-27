<?php
require __DIR__ . '/sink.inc';
$c = new ArrayObject(['n' => 0]);
preempt_handler(function () use ($c) { $c['n']++; }, 0.005);
$spin = function () use ($c) { $c['n'] = 0; $t = microtime(true); while (microtime(true) - $t < 0.1); return $c['n'] >= 3 ? 'interrupted' : 'calls: ' . $c['n']; };
$f = new Fiber(function () use ($spin) {
    $o = new ArrayObject;
    phasync\ext\sapi(function () use ($o, $spin) {
        $o[] = 'request: ' . $spin();
        $cufa = 'call_user_func_array';
        $o[] = 'dynamic call_user_func_array: ' . $cufa($spin, []);
        $a = [2, 1];
        usort($a, function ($x, $y) use ($o, $spin) { $o[] = 'usort comparator: ' . $spin(); return $x <=> $y; });
    }, new Sink);
    echo implode("\n", (array) $o), "\n";
});
$f->start();
?>
