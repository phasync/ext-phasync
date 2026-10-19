--TEST--
sapi(): I/O through call_user_func_array(), call_user_func(), Closure::call() and ReflectionFunction::invoke() still suspends: they keep no C state
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
$cufa = "call_user_func_array"; $cuf = "call_user_func";
c_invoked("call_user_func_array", function ($sleep) use ($cufa) { $cufa($sleep, []); });
c_invoked("call_user_func", function ($sleep) use ($cuf) { $cuf($sleep); });
c_invoked("Closure::call", function ($sleep) { (function () use ($sleep) { $sleep(); })->call(new class {}); });
c_invoked("ReflectionFunction::invoke", function ($sleep) { (new ReflectionFunction($sleep))->invoke(); });
c_invoked("plain call", function ($sleep) { $sleep(); });
?>
--EXPECT--
call_user_func_array: A before, B, A after
call_user_func: A before, B, A after
Closure::call: A before, B, A after
ReflectionFunction::invoke: A before, B, A after
plain call: A before, B, A after
