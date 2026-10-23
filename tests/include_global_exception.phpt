--TEST--
include_global() propagates an uncaught exception from the file to its caller, same as require
--EXTENSIONS--
phasync
--FILE--
<?php
use function phasync\ext\include_global;

function caller()
{
    include_global(__DIR__ . '/include_global_throw.inc');
}

try {
    caller();
} catch (RuntimeException $e) {
    echo "caught: ", $e->getMessage(), "\n";
}
?>
--EXPECT--
caught: from include_global_throw
