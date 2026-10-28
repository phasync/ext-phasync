--TEST--
Suspension::getFiber(): the fiber it suspended; null once the wait is over (#16)
--EXTENSIONS--
phasync
--FILE--
<?php
use phasync\ext\Suspension;

$fibers = $susp = [];
foreach ([1, 2, 3] as $i) {
    $fibers[$i] = Suspension::manage(new Fiber(function () use ($i) { usleep($i * 1000); return $i; }));
    try { $fibers[$i]->start(); } catch (Suspension $s) { $susp[$i] = $s; }
}
foreach ($susp as $i => $s) var_dump($s->getFiber() === $fibers[$i]);

$done = 0;
while ($done < 3) {
    foreach (Suspension::poll($susp, -1) as $i => $s) {
        var_dump($s->getFiber() === $fibers[$i]);
        $f = $s->getFiber();
        $s->resume();
        var_dump($s->getFiber());
        $done += $f->isTerminated();
        unset($susp[$i]);
    }
}
echo "-- fiber resumed directly\n";
$f = Suspension::manage(new Fiber(function () { usleep(1000); }));
try { $f->start(); } catch (Suspension $s) {}
while (!Suspension::poll([$s], -1));
$f->resume();
var_dump($s->getFiber());
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
NULL
bool(true)
NULL
bool(true)
NULL
-- fiber resumed directly
NULL
