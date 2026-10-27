<?php
use function phasync\ext\set_preempt_function;

$calls = 0;
set_preempt_function(function () use (&$calls) { $calls++; }, 0.005);

function spin(): int
{
    global $calls;
    $calls = 0;
    $t = microtime(true);
    while (microtime(true) - $t < 0.1);
    return $calls;
}

pcntl_async_signals(true);
$inHandler = null;
pcntl_signal(SIGUSR1, function () use (&$inHandler) { $inHandler = spin(); });
posix_kill(getmypid(), SIGUSR1);
$t = microtime(true);
while (null === $inHandler && microtime(true) - $t < 2);

echo 'signal handler: calls: ', $inHandler, "\n";
echo 'plain PHP code: ', spin() >= 3 ? 'interrupted' : 'not interrupted', "\n";
?>
