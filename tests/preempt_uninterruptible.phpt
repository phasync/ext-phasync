--TEST--
Preemption: not called while an #[Uninterruptible] function, or anything it calls, runs
--EXTENSIONS--
phasync
--SKIPIF--
<?php require __DIR__ . '/timers.inc'; skip_if_coarse_timers(25); ?>
<?php if (!function_exists('stream_socket_pair')) die('skip requires stream_socket_pair'); ?>
--FILE--
<?php
require __DIR__ . '/preempt.inc';
preempt_main(__DIR__ . '/preempt_uninterruptible.body.inc');
?>
--EXPECT--
a loop in it: calls: 0
a loop in a function it calls: calls: 0
a closure: calls: 0
a method: calls: 0
the same loop outside: interrupted
bool(true)
-- suspended on I/O, others run and are preempted meanwhile
read data, the spinner was preempted meanwhile: yes
