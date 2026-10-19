--TEST--
sapi(): I/O in an error handler runs inline, whichever code raised the error (runtime, C function, compile time)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
$eh = function ($sleep) { set_error_handler(function () use ($sleep) { $sleep(); return true; }); };
c_invoked("engine warning", function ($sleep) use ($eh) { $eh($sleep); $x = $undefined; });
c_invoked("C function warning", function ($sleep) use ($eh) { $eh($sleep); trigger_error('x', E_USER_NOTICE); });
c_invoked("internal function warning", function ($sleep) use ($eh) { $eh($sleep); hex2bin('a'); });
c_invoked("compile time", function ($sleep) use ($eh) { $eh($sleep); eval('function ci_opt($a = 1, $b) {}'); });
c_invoked("handler calls a function", function ($sleep) { set_error_handler(function () use ($sleep) { (function () use ($sleep) { $sleep(); })(); return true; }); $x = $undefined; });
c_invoked("handler removed", function ($sleep) { set_error_handler(fn () => true); restore_error_handler(); $sleep(); });
?>
--EXPECT--
engine warning: A before, A after, B
C function warning: A before, A after, B
internal function warning: A before, A after, B
compile time: A before, A after, B
handler calls a function: A before, A after, B
handler removed: A before, B, A after
