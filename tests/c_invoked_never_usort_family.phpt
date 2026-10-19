--TEST--
sapi(): I/O in a comparator of the usort family runs inline (BG(user_compare_fci) is global)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
c_invoked("uasort", function ($sleep) { $a = [2, 1]; uasort($a, function ($x, $y) use ($sleep) { $sleep(); return $x <=> $y; }); });
c_invoked("uksort", function ($sleep) { $a = [2 => 1, 1 => 1]; uksort($a, function ($x, $y) use ($sleep) { $sleep(); return $x <=> $y; }); });
c_invoked("array_udiff", function ($sleep) { array_udiff([1, 2], [2, 3], function ($x, $y) use ($sleep) { $sleep(); return $x <=> $y; }); });
c_invoked("array_uintersect", function ($sleep) { array_uintersect([1, 2], [2, 3], function ($x, $y) use ($sleep) { $sleep(); return $x <=> $y; }); });
c_invoked("array_diff_ukey", function ($sleep) { array_diff_ukey([1 => 1, 2 => 2], [2 => 1], function ($x, $y) use ($sleep) { $sleep(); return $x <=> $y; }); });
c_invoked("ArrayObject::uasort", function ($sleep) { (new ArrayObject([2, 1]))->uasort(function ($x, $y) use ($sleep) { $sleep(); return $x <=> $y; }); });
?>
--EXPECT--
uasort: A before, A after, B
uksort: A before, A after, B
array_udiff: A before, A after, B
array_uintersect: A before, A after, B
array_diff_ukey: A before, A after, B
ArrayObject::uasort: A before, A after, B
