--TEST--
sapi(): two interleaved requests each run only the shutdown functions they registered, each in its own request (#19)
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$request = fn(string $who) => function () use ($who) {
    register_shutdown_function(function () use ($who) { echo "$who 1 ", $_SERVER['REQUEST_URI'], "\n"; });
    Fiber::suspend();
    register_shutdown_function(function () use ($who) { echo "$who 2\n"; });
};
$a = new Sink; $a->info = ['server' => ['REQUEST_URI' => '/a']];
$b = new Sink; $b->info = ['server' => ['REQUEST_URI' => '/b']];
$fa = new Fiber(fn() => (new Scope)->run(fn() => sapi($request('a'), $a)));
$fb = new Fiber(fn() => (new Scope)->run(fn() => sapi($request('b'), $b)));
$fa->start();
$fb->start();
$fb->resume();
echo "b ended: ", json_encode($b->out), "\n";
$fa->resume();
echo "a ended: ", json_encode($a->out), "\n";
?>
--EXPECT--
b ended: "b 1 \/b\nb 2\n"
a ended: "a 1 \/a\na 2\n"
