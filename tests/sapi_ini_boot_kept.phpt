--TEST--
sapi(): ini_set() outside any request, before the first one, stays in effect
--EXTENSIONS--
phasync
--INI--
precision=14
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;
// Origin: swerve, boot code of a looping swerve.php.
ini_set('precision', '9');
sapi(function () { echo ini_get('precision'), "\n"; ini_set('precision', '3'); }, $s = new Sink);
echo $s->out;
var_dump(ini_get('precision'));
sapi(function () { echo ini_get('precision'), "\n"; }, $s = new Sink);
echo $s->out;
?>
--EXPECT--
9
string(1) "9"
9
