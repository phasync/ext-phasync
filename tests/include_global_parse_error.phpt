--TEST--
include_global() on a file with a parse error throws a catchable ParseError, like require does
--EXTENSIONS--
phasync
--FILE--
<?php
use function phasync\ext\include_global;

try {
    include_global(__DIR__ . '/include_global_parse_error.inc');
    echo "not reached\n";
} catch (ParseError $e) {
    echo "caught ParseError\n";
}
?>
--EXPECT--
caught ParseError
