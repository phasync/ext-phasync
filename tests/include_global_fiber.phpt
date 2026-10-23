--TEST--
include_global() runs in the global scope from inside a Fiber
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
use function phasync\ext\include_global;

$fiber = new Fiber(function () {
    $ret = include_global(__DIR__ . '/include_global_a.inc');
    Fiber::suspend($ret);
});
var_dump($fiber->start());
var_dump($GLOBALS['x']);
var_dump(include_global_reader());
?>
--EXPECTF--
%sinclude_global_a.inc
%stests
string(13) "return-from-a"
string(9) "set-by-a"
string(9) "set-by-a"
