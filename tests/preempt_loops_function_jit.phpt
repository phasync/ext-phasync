--TEST--
Preemption: loops are interrupted between iterations, code without loops is not (function JIT)
--EXTENSIONS--
phasync
--SKIPIF--
<?php require __DIR__ . '/timers.inc'; skip_if_coarse_timers(25); ?>
<?php
if (!extension_loaded('Zend OPcache')) die('skip needs opcache');
?>
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.jit=function
opcache.jit_buffer_size=64M
--FILE--
<?php
require __DIR__ . '/preempt.inc';
preempt_main(__DIR__ . '/preempt_loops_function_jit.body.inc');
?>
--EXPECT--
bool(true)
-- loops are interrupted between iterations
while: interrupted, mid-iteration: 0
for: interrupted, mid-iteration: 0
do-while: interrupted, mid-iteration: 0
foreach in do-while: interrupted, mid-iteration: 0
while(true) with break: interrupted, mid-iteration: 0
while with continue: interrupted, mid-iteration: 0
foreach with continue: interrupted, mid-iteration: 0
nested for: interrupted, mid-iteration: 0
-- code without a loop is not
recursion: calls: 0
array_map: calls: 0
usort: calls: 0
