--TEST--
include_global() throws instead of sharing a variable with the main script's top level
--EXTENSIONS--
phasync
--FILE--
<?php
$shared = 10;   // compiled into the main script's frame, which owns it in the global scope
try {
    phasync\ext\include_global(__DIR__ . '/include_global_shared.inc');
} catch (Error $e) {
    echo str_contains($e->getMessage(), '$shared is a top-level variable') ? 'refused' : $e->getMessage(), "\n";
}
echo "main ", $shared, "\n";
?>
--EXPECT--
refused
main 10
