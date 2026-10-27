--TEST--
set_preempt_function(): interrupts under the tracing JIT, and after opcache.jit=off at run time
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
opcache.jit=tracing
opcache.jit_buffer_size=64M
--FILE--
<?php
require __DIR__ . '/preempt.inc';
phasync\ext\preempt_main(__DIR__ . '/preempt_jit_modes.body.inc');
?>
--EXPECT--
bool(true)
tracing: interrupted
bool(false)
off: interrupted
