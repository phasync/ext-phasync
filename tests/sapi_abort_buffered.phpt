--TEST--
sapi(): a client gone discovered only while flushing a buffered write at the end of the request also unwinds just that request (no second, uncaught bailout)
--EXTENSIONS--
phasync
--INI--
output_buffering=4096
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$s = new Sink; $s->gone = true;
$caught = null;
try {
    $ret = sapi(function () {
        echo "one";   // buffered: write() is not called until the request ends
    }, $s);
} catch (\Throwable $e) {
    $caught = $e;
}
var_dump($s->out, $ret, $caught);
echo "process continues\n";
?>
--EXPECT--
string(3) "one"
NULL
NULL
process continues
