--TEST--
Sleep functions in a coroutine: absurdly long sleeps stay pending instead of overflowing; time_nanosleep() validates nanoseconds like native
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
use phasync\ext\Suspension;
// A sleep far past any real deadline must stay pending (an overflow made them end at once).
function pending(string $label, Closure $sleep): void
{
    $f = Suspension::manage(new Fiber($sleep));
    try {
        $f->start();
        echo "$label: returned\n";
        return;
    } catch (Suspension $s) {}
    echo "$label: ", Suspension::poll([$s], 0.05) ? 'ready' : 'pending', "\n";
    try { $s->throw(new LogicException('cancelled')); } catch (LogicException) {}
}
pending('sleep(PHP_INT_MAX)', fn() => sleep(PHP_INT_MAX));             // seconds -> µs used to overflow
pending('time_nanosleep(PHP_INT_MAX)', fn() => time_nanosleep(PHP_INT_MAX, 999999999));
pending('time_sleep_until(1e300)', fn() => time_sleep_until(1e300));   // double -> long was out of range

$f = Suspension::manage(new Fiber(function () {
    try {
        time_nanosleep(0, 1000000000);
    } catch (ValueError $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
    }
}));
$f->start();
?>
--EXPECT--
sleep(PHP_INT_MAX): pending
time_nanosleep(PHP_INT_MAX): pending
time_sleep_until(1e300): pending
ValueError: Nanoseconds was not in the range 0 to 999 999 999 or seconds was negative
