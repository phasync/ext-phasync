<?php

function spin(float $s): void { $t = microtime(true); while (microtime(true) - $t < $s); }

$calls = [];
$where = 'other';
preempt_handler(function () use (&$calls, &$where) {
    $calls[] = $where;
}, 0.02);

echo "-- signals\n";
pcntl_async_signals(true);
$signals = 0;
pcntl_signal(SIGUSR1, function () use (&$signals, &$where) {
    $signals++;
    $where = 'signal';
    spin(0.2);             // fiber switching is blocked in a handler: skipped
    $where = 'other';
});
// The spinning runs in the managed fiber preempt_main() runs: its loop counts the preemptions.
for ($i = 0; $i < 3; $i++) {
    exec('kill -USR1 ' . getmypid());
    spin(0.1);
}
var_dump($signals, count($calls) > 0, in_array('signal', $calls, true));

echo "-- time limit\n";
$calls = [];
register_shutdown_function(function () use (&$calls) {
    var_dump(count($calls) > 10);
});
set_time_limit(1);
while (true);
?>
