--TEST--
include_global() runs in the global scope inside phasync\ext\sapi(), seeing that request's own superglobals
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;
use function phasync\ext\include_global;

$a = new Sink;
$a->info = ['method' => 'GET', 'query_string' => ''];

sapi(function () {
    $ret = include_global(__DIR__ . '/include_global_a.inc');
    var_dump($ret);
    var_dump($GLOBALS['x']);
    var_dump(include_global_reader());
}, $a);
?>
--EXPECTF--
%sinclude_global_a.inc
%stests
string(13) "return-from-a"
string(9) "set-by-a"
string(9) "set-by-a"
