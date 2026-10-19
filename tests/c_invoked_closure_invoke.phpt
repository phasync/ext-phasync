--TEST--
sapi(): I/O inside a closure called through Closure::__invoke() suspends like $closure() (#106)
--EXTENSIONS--
phasync
--FILE--
<?php
// Origin: Laravel's ClosureValidationRule::passes() calls $this->callback->__invoke().
require __DIR__ . '/c_invoked.inc';
c_invoked("__invoke", function ($sleep) { $sleep->__invoke(); });
c_invoked("__invoke with args", function ($sleep) { (function ($f, $x) { $f(); })->__invoke($sleep, 1); });
c_invoked("callable string", function ($sleep) { call_user_func([$sleep, '__invoke']); });
c_invoked("plain call", function ($sleep) { $sleep(); });
?>
--EXPECT--
__invoke: A before, B, A after
__invoke with args: A before, B, A after
callable string: A before, B, A after
plain call: A before, B, A after
