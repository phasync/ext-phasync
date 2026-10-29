--TEST--
sapi(): $_SESSION assigned without session_start() survives the request's fiber switching out and back, in a Scope and without (swerve#104)
--EXTENSIONS--
phasync
session
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

function req(): Fiber
{
    return new Fiber(function () {
        sapi(function () {
            $_SESSION = ['user' => 7];
            Fiber::suspend();
            echo json_encode($_SESSION ?? null);
        }, $s = new Sink);
        echo $s->out, "\n";
    });
}

echo "-- no Scope\n";
$f = req();
$f->start();
$f->resume();

echo "-- in a Scope, while another Scope runs in between\n";
$a = new Scope; $b = new Scope;
$fa = null;
$f = new Fiber($a->bind(function () use (&$fa) { $fa = req(); $fa->start(); }));
$f->start();
$b->run(function () { $_SESSION = ['other' => 1]; });
$a->run(fn () => $fa->resume());
?>
--EXPECT--
-- no Scope
{"user":7}
-- in a Scope, while another Scope runs in between
{"user":7}
