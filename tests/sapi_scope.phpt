--TEST--
sapi() and Scope are independent and compose: sapi() works in the root scope, concurrent sapi() calls in one scope keep their own output and headers, output stays per sapi() call across host-created scopes, and a Scope created inside a request does not touch the request
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$x = fn($h) => array_values(preg_grep('/^X-(?!Powered)/', $h));
$root = Scope::getRootScope();

// 1. The root scope: no Scope of its own.
$s = new Sink;
sapi(function () use ($root, &$inside) {
    $inside = Scope::getCurrent() === $root;
    echo "root\n";
}, $s);
var_dump($inside, $s->out, Scope::getCurrent() === $root);

// 2. Two concurrent sapi() calls in one scope (the root, then a created one).
$run = function (Scope $expect) use ($x) {
    $mk = function (string $who) use ($expect) {
        $s = new Sink;
        $f = new Fiber(fn() => sapi(function () use ($who, $expect) {
            header("X-Who: $who");
            http_response_code($who === 'a' ? 201 : 202);
            echo "$who-1 ";
            Fiber::suspend();
            echo "$who-2 " . (Scope::getCurrent() === $expect ? 'same-scope' : 'OTHER-SCOPE');
        }, $s));
        return [$f, $s];
    };
    [$fa, $a] = $mk('a');
    [$fb, $b] = $mk('b');
    $fa->start(); $fb->start(); $fb->resume(); $fa->resume();
    var_dump($a->out, $a->headers[0], $x($a->headers[2]), $b->out, $b->headers[0], $x($b->headers[2]));
};
$run($root);
$scope = new Scope;
$scope->run($run, $scope);

// 3. sapi() inside host-created scopes: output stays per sapi() call.
$serve = function (Scope $sc, string $who) {
    $s = new Sink;
    $f = $sc->run(fn() => new Fiber(function () use ($sc, $who, $s) {
        sapi(function () use ($sc, $who) {
            echo "$who:1";
            Fiber::suspend();
            echo ':2:' . (Scope::getCurrent() === $sc ? 'ok' : 'bad');
        }, $s);
    }));
    return [$f, $s];
};
[$f1, $s1] = $serve(new Scope, 'one');
[$f2, $s2] = $serve(new Scope, 'two');
$f1->start(); $f2->start(); $f2->resume(); $f1->resume();
var_dump($s1->out, $s2->out);

// 4. A Scope created inside a request does not touch the request's output or headers.
$s = new Sink;
sapi(function () {
    header('X-Out: 1');
    (new Scope)->run(function () {
        header('X-In: scope');
        echo "in-scope ";
    });
    echo "after";
}, $s);
var_dump($s->out, $x($s->headers[2]));
?>
--EXPECT--
bool(true)
string(5) "root
"
bool(true)
string(18) "a-1 a-2 same-scope"
int(201)
array(1) {
  [0]=>
  string(8) "X-Who: a"
}
string(18) "b-1 b-2 same-scope"
int(202)
array(1) {
  [0]=>
  string(8) "X-Who: b"
}
string(18) "a-1 a-2 same-scope"
int(201)
array(1) {
  [0]=>
  string(8) "X-Who: a"
}
string(18) "b-1 b-2 same-scope"
int(202)
array(1) {
  [0]=>
  string(8) "X-Who: b"
}
string(10) "one:1:2:ok"
string(10) "two:1:2:ok"
string(14) "in-scope after"
array(2) {
  [0]=>
  string(8) "X-Out: 1"
  [1]=>
  string(11) "X-In: scope"
}
