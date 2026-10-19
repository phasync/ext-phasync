--TEST--
set_preempt_function(): not called in never-list callbacks (usort, error handlers ...), nor in destructors or handlers (function JIT)
--EXTENSIONS--
phasync
session
--SKIPIF--
<?php require __DIR__ . '/timers.inc'; skip_if_coarse_timers(25); ?>
<?php if (!extension_loaded('Zend OPcache')) die('skip needs opcache'); ?>
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.jit=function
opcache.jit_buffer_size=64M
session.use_cookies=0
session.cache_limiter=
session.save_path=
--FILE--
<?php require __DIR__ . '/preempt_called_from_c.inc';
--EXPECT--
session read handler: interrupted
plain PHP code: interrupted
usort comparator: calls: 0
array_map callback: interrupted
output handler: calls: 0
error handler: calls: 0
error handler: calls: 0
destructor: calls: 0
__get: calls: 0
Iterator::current() in foreach: interrupted
stream wrapper read: interrupted
autoloader: calls: 0
call_user_func_array() called dynamically: interrupted
a function called from PHP code: interrupted
exception handler: calls: 0
shutdown function: calls: 0
