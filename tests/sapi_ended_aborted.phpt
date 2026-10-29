--TEST--
sapi(): once sapi() has returned, a fiber of the request still running sees connection_aborted() true and its output discarded (swerve#98)
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$s = new Sink; $late = null; $seen = [];
sapi(function () use (&$late, &$seen) {
    $late = new Fiber(function () use (&$seen) {
        $seen[] = 'during: ' . connection_aborted() . '/' . connection_status();
        Fiber::suspend();
        echo "late output";
        $seen[] = 'after: ' . connection_aborted() . '/' . connection_status();
    });
    $late->start();
    echo "response";
}, $s);
$late->resume();
echo implode("\n", $seen), "\n";
var_dump($s->out);
// The worker's own connection status is untouched.
var_dump(connection_aborted(), connection_status());
?>
--EXPECT--
during: 0/0
after: 1/1
string(8) "response"
int(0)
int(0)
