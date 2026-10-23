--TEST--
include_global() runs a file in the global scope even when called from inside a function, with require's other semantics (return value, __FILE__/__DIR__, same scope on a second call)
--EXTENSIONS--
phasync
--FILE--
<?php
use function phasync\ext\include_global;

function caller()
{
    // Called from inside a function: a plain `require` here would make $x
    // and the function below local to caller(), not global.
    $ret = include_global(__DIR__ . '/include_global_a.inc');
    var_dump($ret);
}
caller();

var_dump($GLOBALS['x']);
var_dump(include_global_reader());

function caller2()
{
    global $x;
    include_global(__DIR__ . '/include_global_b.inc');
    // include_global_b.inc set $y from the very $x caller() set above: the
    // same global scope, not a fresh one.
    global $y;
    var_dump($y);
}
caller2();
?>
--EXPECTF--
%sinclude_global_a.inc
%stests
string(13) "return-from-a"
string(8) "set-by-a"
string(8) "set-by-a"
string(18) "set-by-a-seen-by-b"
