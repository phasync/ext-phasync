--TEST--
phasync.keywords=1: run, begin and finish rewrite to \phasync::run(), go() and await(); closure literals and calls (the last call of a chain runs in the fiber, its arguments follow it); finish binds like clone
--EXTENSIONS--
phasync
--INI--
phasync.keywords=1
--FILE--
<?php
require __DIR__ . '/keywords.inc';
function foo($a, $b = 'B') { return "foo($a,$b)"; }
function bar() { return 'bar'; }
class O { function m(...$x) { return 'm(' . json_encode($x) . ')'; } function self() { return $this; } }
$o = new O;
$m = 'M';

echo begin fn() => "arrow $m", "\n";
echo begin function () use ($m) { return "closure $m"; }, "\n";
echo begin static fn() => 'static arrow', "\n";
echo begin foo(1, 2), "\n";
echo begin bar(), "\n";
echo begin $o->m(x: 1), "\n";
echo begin $o->self()->m(1, ...[2, 3]), "\n";
echo run function () { return 'run closure'; }, "\n";
echo run foo('r'), "\n";
$f = 'F';
echo finish $f, "\n";
echo finish begin bar(), "\n";
echo finish $o->m(), "\n";
echo [begin fn() => 1, 'arrow body ends at the comma'][1], "\n";
echo $o->m(begin fn() => $m ? 'its own ternary' : 'no'), "\n";
echo $m ? begin fn() => 'ends at the colon' : 'no', "\n";
echo begin
    bar(), "\n";
echo __LINE__, "\n";                      // nothing is inserted across lines
?>
--EXPECT--
go(arrow M)
go(closure M)
go(static arrow)
go(foo(1,2))
go(bar)
go(m({"x":1}))
go(m([1,2,3]))
run(run closure)
run(foo(r,B))
await(F)
await(go(bar))
await(m([]))
arrow body ends at the comma
m(["go(its own ternary)"])
go(ends at the colon)
go(bar)
27
