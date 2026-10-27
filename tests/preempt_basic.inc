<?php

function threads(): int { return count(scandir('/proc/self/task')) - 2; }
// A joined thread's /proc/self/task entry can linger briefly: wait up to 200ms.
function threadsBecome(int $want): bool {
    for ($i = 0; threads() !== $want && $i < 200; $i++) usleep(1000);
    return threads() === $want;
}

echo "-- none set\n";
$base = threads();
var_dump(preempt_handler(null));
var_dump(threads() === $base);

echo "-- about every interval\n";
$n = 0;
$a = function () use (&$n) { $n++; };
var_dump(preempt_handler($a, 0.1));
var_dump(threads() === $base + 1);
$t = microtime(true);
while (microtime(true) - $t < 1.0);
echo $n >= 5 && $n <= 10 ? "ok\n" : "calls: $n\n";

echo "-- loop without calls\n";
$n = 0;
preempt_handler($a, 0.02);
$i = 0;
while ($n < 3) { $i++; }
var_dump($i > 0);

echo "-- returns previous, null removes\n";
$b = function () {};
var_dump(preempt_handler($b) === $a);
var_dump(preempt_handler(null) === $b);
var_dump(preempt_handler(null));
var_dump(threadsBecome($base));
$n = 0;
$t = microtime(true);
while (microtime(true) - $t < 0.3);
var_dump($n);

echo "-- exception at the interrupted point\n";
function spin(): array
{
    $i = 0;
    try {
        while (true) { $i++; }
    } catch (RuntimeException $e) {
        return [$e->getMessage(), $i > 0];
    }
}
function spinCalls(): int
{
    $s = '';
    while (true) { $s = str_repeat('x', 100) . $s; $s = substr($s, 0, 1000); }
}
$armed = true;
preempt_handler(function () use (&$armed) {
    if ($armed) {
        $armed = false;
        throw new RuntimeException('preempted');
    }
}, 0.02);
var_dump(spin());
$armed = true;
try {
    spinCalls();
} catch (RuntimeException $e) {
    echo "caught in caller: ", $e->getMessage(), "\n";
}
preempt_handler(null);

echo "-- a managed fiber is preempted with a ready Suspension, resumed by its loop\n";
use phasync\ext\Suspension;
$count = 0;
Suspension::setPreemptInterval(0.02);
$f = Suspension::manage(new Fiber(function () use (&$count) {
    $sum = 0; $x = 7; $s = '';
    for ($i = 1; ; $i++) {
        $sum += $i;
        $x = ($x * 31 + $i) & 0xffffff;
        $s = substr($s . chr(65 + $i % 26), -26);
        if ($count >= 5) break;
    }
    $sum2 = 0; $y = 7; $s2 = '';
    for ($j = 1; $j <= $i; $j++) {
        $sum2 += $j;
        $y = ($y * 31 + $j) & 0xffffff;
        $s2 = substr($s2 . chr(65 + $j % 26), -26);
    }
    return $sum === $sum2 && $x === $y && $s === $s2;
}));
$run = fn() => $f->start();
$allReady = true;
for (;;) {
    try {
        $run();
        break;
    } catch (Suspension $susp) {
        $allReady = $allReady && $susp->isReady();
        $count++;
        $t = microtime(true);
        while (microtime(true) - $t < 0.05);   // the loop's own code, outside the fiber
        $run = fn() => $susp->resume();
    }
}
var_dump($f->getReturn(), $count >= 5, $allReady);
Suspension::setPreemptInterval(null);

echo "-- bad interval\n";
foreach ([0.0, -1.0, INF, NAN] as $bad) {
    try { Suspension::setPreemptInterval($bad); } catch (ValueError $e) { echo $e->getMessage(), "\n"; }
}
var_dump(preempt_handler(null));
