--TEST--
sapi(): two interleaved coroutines each keep their own output buffers, ob level and header()/http_response_code()/setcookie()
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--INI--
expose_php=0
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$a = new Sink; $b = new Sink;
$fa = new Fiber(fn() => sapi(function () {
    ob_start();
    echo "a1 ";
    header('X-Who: a');
    setcookie('who', 'a');
    http_response_code(201);
    Fiber::suspend();
    echo "a2 ", ob_get_level(), " ", http_response_code();
    $got = ob_get_clean();
    echo strtoupper($got);
}, $a));
$fb = new Fiber(fn() => sapi(function () {
    ob_start();
    ob_start();
    echo "b1 ";
    header('X-Who: b');
    setcookie('who', 'b');
    http_response_code(202);
    Fiber::suspend();
    echo "b2 ", ob_get_level(), " ", http_response_code();
    ob_end_flush();
    ob_end_flush();
}, $b));
$fa->start(); $fb->start();
var_dump(ob_get_level(), headers_list(), http_response_code());
$fa->resume(); $fb->resume();
var_dump($a->out, $b->out, $a->headers[0], $b->headers[0]);
var_dump(preg_grep('/^(X-Who|Set-Cookie)/', $a->headers[2]), preg_grep('/^(X-Who|Set-Cookie)/', $b->headers[2]));
?>
--EXPECTF--
int(0)
array(0) {
}
bool(false)
string(11) "A1 A2 1 201"
string(11) "b1 b2 2 202"
int(201)
int(202)
array(2) {
  [0]=>
  string(8) "X-Who: a"
  [1]=>
  string(17) "Set-Cookie: who=a"
}
array(2) {
  [0]=>
  string(8) "X-Who: b"
  [1]=>
  string(17) "Set-Cookie: who=b"
}
