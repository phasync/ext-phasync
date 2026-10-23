<?php
// Fixture for include_global tests: sets a top-level variable, defines a
// function that reads it with `global`, prints __FILE__/__DIR__, and
// returns a value -- like any ordinary entry script would.
$x = 'set-by-a';
function include_global_reader(): string
{
    global $x;
    return $x;
}
echo __FILE__, "\n";
echo __DIR__, "\n";
return 'return-from-a';
