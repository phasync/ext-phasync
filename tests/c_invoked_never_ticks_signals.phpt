--TEST--
sapi(): I/O in a tick function and in a pcntl signal handler runs inline (Fiber::suspend() is blocked there)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('pcntl', 'posix')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child('require ' . var_export(__DIR__ . '/c_invoked.inc', true) . ";\n" . <<<'CHILD'
declare(ticks=1);
c_invoked("tick function", function ($sleep) { $done = false; register_tick_function($t = function () use ($sleep, &$done) { if ($done) return; $done = true; $sleep(); }); $x = 1; $x = 2; });
c_invoked("signal handler", function ($sleep) { pcntl_async_signals(true); pcntl_signal(SIGUSR1, function () use ($sleep) { $sleep(); }); posix_kill(getmypid(), SIGUSR1); $x = 1; });
CHILD);
?>
--EXPECT--
tick function: A before, A after, B
signal handler: A before, A after, B
