--TEST--
Preemption: not called in a pcntl signal handler, which the engine calls between opcodes
--EXTENSIONS--
phasync
pcntl
posix
--SKIPIF--
<?php require __DIR__ . '/timers.inc'; skip_if_coarse_timers(25); ?>
--INI--
opcache.enable_cli=0
--FILE--
<?php
require __DIR__ . '/preempt.inc';
preempt_main(__DIR__ . '/preempt_signal_handler.body.inc');
?>
--EXPECT--
signal handler: calls: 0
plain PHP code: interrupted
