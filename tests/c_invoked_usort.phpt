--TEST--
sapi(): I/O in a usort() comparator runs inline
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
c_invoked("usort", function ($sleep) { $a = [2, 1]; usort($a, function ($x, $y) use ($sleep) { $sleep(); return $x <=> $y; }); });
?>
--EXPECT--
usort: A before, A after, B
