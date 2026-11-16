--TEST--
sapi(): ini values are not changed while any request runs; restored when the last ends
--EXTENSIONS--
phasync
--INI--
memory_limit=128M
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;
$sa = new Sink; $sb = new Sink;
$fa = new Fiber(fn() => sapi(function () {
    ini_set('memory_limit', '99M');
    Fiber::suspend();
    echo 'A:', ini_get('memory_limit'), "\n";
}, $sa));
$fb = new Fiber(fn() => sapi(function () { echo 'B:', ini_get('memory_limit'), "\n"; }, $sb));
$fa->start();
$fb->start();
var_dump(ini_get('memory_limit'));
$fa->resume();
echo $sb->out, $sa->out;
var_dump(ini_get('memory_limit'));
?>
--EXPECT--
string(3) "99M"
B:99M
A:99M
string(4) "128M"
