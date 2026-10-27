--TEST--
Sleep functions in a coroutine suspend it for their own time and return what they return natively; outside one they sleep for real
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
// In a coroutine the sleep functions suspend it: the other coroutine runs meanwhile,
// and each returns what it returns natively after its own time.
$loop = new Loop;
$log = [];
$t = microtime(true);
$loop->runAll(function () use (&$log) {
    var_dump(sleep(1));                                  // int(0)
    usleep(100000);
    var_dump(time_nanosleep(0, 100000000));              // bool(true)
    var_dump(time_sleep_until(microtime(true) + 0.1));   // bool(true)
    $log[] = 'sleeper';
}, function () use (&$log) {
    $log[] = 'other';
});
$took = microtime(true) - $t;
echo implode(', ', $log), "\n";
var_dump($loop->parks, $took >= 1.29 && $took < 2);

// Outside a coroutine: a real sleep.
$t = microtime(true);
usleep(1000);
var_dump(microtime(true) - $t >= 0.0005);
?>
--EXPECT--
int(0)
bool(true)
bool(true)
other, sleeper
int(4)
bool(true)
bool(true)
