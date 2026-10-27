--TEST--
Preemption: a fork()ed child keeps the closure and has a timer of its own
--EXTENSIONS--
phasync
pcntl
--SKIPIF--
<?php if (!is_dir('/proc/self/task')) die('skip needs /proc'); ?>
--FILE--
<?php
require __DIR__ . '/preempt.inc';
preempt_main(__DIR__ . '/preempt_fork.body.inc');
?>
--EXPECT--
child: ok, threads: 1
bool(true)
bool(true)
int(0)
parent: ok, threads: 1
-- none set: the child has no timer
child threads: 0
