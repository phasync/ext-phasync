--TEST--
set_preempt_function(): pcntl_async_signals() and set_time_limit() keep working
--EXTENSIONS--
phasync
pcntl
--FILE--
<?php
require __DIR__ . '/preempt.inc';
phasync\ext\preempt_main(__DIR__ . '/preempt_pcntl_time_limit.body.inc');
?>
--EXPECTF--
-- signals
int(3)
bool(true)
bool(false)
-- time limit

Fatal error: Maximum execution time of 1 second exceeded in %s on line %d
bool(true)
