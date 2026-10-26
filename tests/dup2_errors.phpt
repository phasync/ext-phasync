--TEST--
dup2(): a negative fd throws ValueError; a non-stream or fd-less stream throws TypeError
--EXTENSIONS--
phasync
--FILE--
<?php
try {
    phasync\ext\dup2(STDOUT, -1);
} catch (\ValueError $e) {
    echo "ValueError: ", $e->getMessage(), "\n";
}
try {
    phasync\ext\dup2(5, 7);
} catch (\TypeError $e) {
    echo "TypeError: ", $e->getMessage(), "\n";
}
$f = fopen('php://memory', 'r+');
fclose($f);
try {
    phasync\ext\dup2($f, 7);
} catch (\TypeError $e) {
    echo "TypeError: ", $e->getMessage(), "\n";
}
$m = fopen('php://memory', 'r+');
try {
    phasync\ext\dup2($m, 7);
} catch (\TypeError $e) {
    echo "TypeError: ", $e->getMessage(), "\n";
}
?>
--EXPECT--
ValueError: phasync\ext\dup2(): Argument #2 ($fd) must not be negative
TypeError: phasync\ext\dup2(): Argument #1 ($stream) must be of type resource, int given
TypeError: phasync\ext\dup2(): supplied resource is not a valid stream resource
TypeError: phasync\ext\dup2(): Argument #1 ($stream) must be a stream with a file descriptor
