--TEST--
sapi(): an ini entry modified before the request and again inside it returns to the pre-request value
--EXTENSIONS--
phasync
--INI--
precision=14
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;
ini_set('precision', '9');
sapi(function () { ini_set('precision', '3'); ini_restore('precision'); ini_set('precision', '4'); }, new Sink);
var_dump(ini_get('precision'));
sapi(function () { ini_restore('precision'); }, new Sink);
var_dump(ini_get('precision'));
?>
--EXPECT--
string(1) "9"
string(1) "9"
