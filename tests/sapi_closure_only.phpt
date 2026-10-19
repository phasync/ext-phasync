--TEST--
sapi(): $code is a Closure; a file path or any other callable is a TypeError
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

foreach ([__FILE__, 'strlen', [new Sink, 'flush']] as $code) {
    try { sapi($code, new class extends Sink { public function name(): string { return 'x'; } }); }
    catch (TypeError $e) { echo get_debug_type($code), ": TypeError\n"; }
}
?>
--EXPECT--
string: TypeError
string: TypeError
array: TypeError
