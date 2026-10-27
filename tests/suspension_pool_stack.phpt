--TEST--
Suspension pool: a wait reuses the most recently freed Suspension; one the loop still holds is never reused; a dropped one's WeakReferences clear at once
--EXTENSIONS--
phasync
--FILE--
<?php
use phasync\ext\Suspension;

// One wait: the Suspension a usleep() in a managed fiber throws out of start().
function waiting(): array {
    $f = Suspension::manage(new Fiber(fn() => usleep(1000)));
    try { $f->start(); } catch (Suspension $s) { return [$f, $s]; }
    throw new Error('no wait');
}
function finish(Suspension $s): void {
    while (!Suspension::poll([$s], -1));
    $s->resume();
}

// 1. Most recently freed first.
[, $a] = waiting();
[, $b] = waiting();
$ida = spl_object_id($a); $idb = spl_object_id($b);
finish($a); finish($b);
unset($a); unset($b);                    // b is freed last
[, $c] = waiting();
var_dump(spl_object_id($c) === $idb);
finish($c); unset($c);

// 2. Held by the loop: never handed out again, though its wait is over.
[, $held] = waiting();
finish($held);
[, $d] = waiting();
var_dump($d !== $held);
finish($d); unset($d);

// 3. Dropped: as freed for PHP code, before any reuse.
[, $e] = waiting();
$w = WeakReference::create($e);
$map = new WeakMap; $map[$e] = 'x';
finish($e); unset($e);
var_dump($w->get(), count($map));

// 4. Sequential waits don't grow the pool.
$ids = [];
for ($i = 0; $i < 1000; $i++) {
    [, $s] = waiting();
    $ids[spl_object_id($s)] = true;
    finish($s); unset($s);
}
var_dump(count($ids));
?>
--EXPECT--
bool(true)
bool(true)
NULL
int(0)
int(1)
