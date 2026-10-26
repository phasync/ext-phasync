--TEST--
sapi(): a client gone (ignore_user_abort off) unwinds only the aborted request -- no catch, no finally, like exit() -- then sapi() returns normally and the caller continues
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$s = new Sink; $s->gone = true;
$ranFinally = false;
$seen = null;
$caught = null;
try {
    $ret = sapi(function () use (&$ranFinally) {
        try {
            echo "one";
            echo "two";          // never: the first write already aborted
        } finally {
            $ranFinally = true;  // never: a bailout skips finally too
        }
    }, $s);
} catch (\Throwable $e) {
    $caught = $e;
}
$seen = 'code after sapi() ran';

var_dump($s->out, $ranFinally, $caught, $ret, $seen);
?>
--EXPECT--
string(3) "one"
bool(false)
NULL
NULL
string(21) "code after sapi() ran"
