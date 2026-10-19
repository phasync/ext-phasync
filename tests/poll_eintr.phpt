--TEST--
Poller::poll() returns when a signal interrupts the wait, without an error
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!function_exists('pcntl_alarm') || !function_exists('pcntl_async_signals')) die('skip requires pcntl');
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
pcntl_async_signals(true);
pcntl_signal(SIGALRM, fn() => print("signal handled\n"));

$loop = new Loop;
pcntl_alarm(1);
$t = microtime(true);
$loop->poller->poll(5);                  // nothing armed: sleeps until the signal
var_dump(microtime(true) - $t < 3);      // interrupted, not a full 5 s wait

// A wait shorter than a millisecond is still a wait, not a zero-time poll.
$t = hrtime(true);
$loop->poller->poll(0.0008);
$ms = (hrtime(true) - $t) / 1e6;
var_dump($ms >= 0.7 && $ms < 50);
?>
--EXPECT--
signal handled
bool(true)
bool(true)
