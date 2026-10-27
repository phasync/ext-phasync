--TEST--
set_preempt_function(): loops that store nothing to memory see the flag each iteration under the tracing JIT (no hoisted load)
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!extension_loaded('Zend OPcache')) die('skip needs opcache'); ?>
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.jit=tracing
opcache.jit_buffer_size=64M
--FILE--
<?php
require __DIR__ . '/preempt.inc';
phasync\ext\preempt_main(__DIR__ . '/preempt_store_free.body.inc');
?>
--EXPECT--
tight: called
nested: called
dowhile: called
bool(true)
bool(true)
bool(true)
