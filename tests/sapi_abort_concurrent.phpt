--TEST--
sapi(): one request's client gone does not touch a second request running concurrently in another fiber
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$a = new Sink; $a->gone = true;
$b = new Sink;
$fa = new Fiber(fn() => sapi(function () {
    echo "a1";
    echo "a2";          // never: a's first write already aborted
    return 'done-a';
}, $a));
$fb = new Fiber(fn() => sapi(function () {
    echo "b1";
    Fiber::suspend();
    echo "b2";
    return 'done-b';
}, $b));

$fa->start();
$fb->start();
var_dump($fa->isTerminated(), $fb->isTerminated());
$fb->resume();
var_dump($fa->isTerminated(), $fb->isTerminated());
var_dump($fa->getReturn(), $fb->getReturn());
var_dump($a->out, $b->out);
echo "process continues\n";
?>
--EXPECT--
bool(true)
bool(false)
bool(true)
bool(true)
NULL
string(6) "done-b"
string(2) "a1"
string(4) "b1b2"
process continues
