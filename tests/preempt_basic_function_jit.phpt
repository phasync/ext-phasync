--TEST--
Preemption: interval, removal, previous, exceptions, suspension (function JIT)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!is_dir('/proc/self/task')) die('skip needs /proc');
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
preempt_main(__DIR__ . '/preempt_basic_function_jit.body.inc');
?>
--EXPECT--
bool(true)
-- none set
NULL
bool(true)
-- about every interval
NULL
bool(true)
ok
-- loop without calls
bool(true)
-- returns previous, null removes
bool(true)
bool(true)
NULL
bool(true)
int(0)
-- exception at the interrupted point
array(2) {
  [0]=>
  string(9) "preempted"
  [1]=>
  bool(true)
}
caught in caller: preempted
-- a managed fiber is preempted with a ready Suspension, resumed by its loop
bool(true)
bool(true)
bool(true)
-- bad interval
phasync\ext\Suspension::setPreemptInterval(): Argument #1 ($seconds) must be a positive number of seconds or null
phasync\ext\Suspension::setPreemptInterval(): Argument #1 ($seconds) must be a positive number of seconds or null
phasync\ext\Suspension::setPreemptInterval(): Argument #1 ($seconds) must be a positive number of seconds or null
phasync\ext\Suspension::setPreemptInterval(): Argument #1 ($seconds) must be a positive number of seconds or null
NULL
