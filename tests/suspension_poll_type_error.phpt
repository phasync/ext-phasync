--TEST--
Suspension::poll() throws a TypeError for an array element that isn't a Suspension, wherever it is, and returns nothing
--EXTENSIONS--
phasync
--FILE--
<?php
use phasync\ext\Suspension;
foreach ([[new stdClass], ['a' => 1], [2 => 'x']] as $arg) {
    try {
        var_dump(Suspension::poll($arg, 0));
    } catch (TypeError $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
    }
}
var_dump(Suspension::poll([], 0));
?>
--EXPECT--
TypeError: phasync\ext\Suspension::poll(): Argument #1 ($suspensions) must contain only phasync\ext\Suspension objects, stdClass given
TypeError: phasync\ext\Suspension::poll(): Argument #1 ($suspensions) must contain only phasync\ext\Suspension objects, int given
TypeError: phasync\ext\Suspension::poll(): Argument #1 ($suspensions) must contain only phasync\ext\Suspension objects, string given
array(0) {
}
