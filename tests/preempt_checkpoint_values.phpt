--TEST--
set_preempt_function(): results stay right under the function JIT (php/php-src#23983, #26), and the checkpoint shows in no backtrace
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!extension_loaded('Zend OPcache')) die('skip needs opcache'); ?>
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.jit=function
opcache.jit_buffer_size=64M
--FILE--
<?php
require __DIR__ . '/preempt.inc';
phasync\ext\preempt_main(__DIR__ . '/preempt_checkpoint_values.body.inc');
?>
--EXPECTF--
bool(true)
bool(true)
int(13961473)
#0 %s(%d): {closure%S}()
#1 %s(%d): loop()
from the closure at line %d
#0 %s(%d): {closure%S}()
#1 %s(%d): loop()
#2 {main}
bool(true)
bool(false)
bool(true)
