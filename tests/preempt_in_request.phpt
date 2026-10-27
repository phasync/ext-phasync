--TEST--
Preemption reaches PHP code inside a sapi() request, and through call_user_func_array(), as in the worker's own code
--EXTENSIONS--
phasync
--SKIPIF--
<?php require __DIR__ . '/timers.inc'; skip_if_coarse_timers(25); ?>
--FILE--
<?php
require __DIR__ . '/preempt.inc';
preempt_main(__DIR__ . '/preempt_in_request.body.inc');
?>
--EXPECT--
request: interrupted
dynamic call_user_func_array: interrupted
usort comparator: calls: 0
