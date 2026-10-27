--TEST--
Preemption: loops are interrupted between iterations, code without loops is not (tracing JIT)
--EXTENSIONS--
phasync
--SKIPIF--
<?php require __DIR__ . '/timers.inc'; skip_if_coarse_timers(25); ?>
<?php if (!extension_loaded('Zend OPcache')) die('skip needs opcache'); ?>
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.jit=tracing
opcache.jit_buffer_size=64M
--FILE--
<?php
require __DIR__ . '/preempt.inc';
preempt_main(__DIR__ . '/preempt_loops_tracing.body.inc');
?>
--EXPECT--
bool(true)
while: interrupted, mid-iteration: 0
for: interrupted, mid-iteration: 0
do-while: interrupted, mid-iteration: 0
foreach with continue, nested: interrupted, mid-iteration: 0
recursion: calls: 0
