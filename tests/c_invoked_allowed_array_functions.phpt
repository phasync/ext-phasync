--TEST--
sapi(): I/O in callbacks of array_map(), array_filter(), array_walk(), array_reduce(), iterator_*() and preg_replace_callback() suspends
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
c_invoked("array_map", function ($sleep) { array_map(function ($x) use ($sleep) { $sleep(); }, [1]); });
c_invoked("array_filter", function ($sleep) { array_filter([1], function ($x) use ($sleep) { $sleep(); return true; }); });
c_invoked("array_walk", function ($sleep) { $a = [1]; array_walk($a, function ($x) use ($sleep) { $sleep(); }); });
c_invoked("array_reduce", function ($sleep) { array_reduce([1], function ($c, $x) use ($sleep) { $sleep(); return $c; }); });
c_invoked("iterator_to_array", function ($sleep) { iterator_to_array(new CallbackFilterIterator(new ArrayIterator([1]), function () use ($sleep) { $sleep(); return true; })); });
c_invoked("iterator_apply", function ($sleep) { $it = new ArrayIterator([1]); iterator_apply($it, function () use ($sleep) { $sleep(); return true; }); });
c_invoked("preg_replace_callback", function ($sleep) { preg_replace_callback('/a/', function ($m) use ($sleep) { $sleep(); return 'b'; }, 'a'); });
c_invoked("nested array_map", function ($sleep) { array_map(fn ($x) => array_map(function ($y) use ($sleep) { $sleep(); }, [$x]), [1]); });
?>
--EXPECT--
array_map: A before, B, A after
array_filter: A before, B, A after
array_walk: A before, B, A after
array_reduce: A before, B, A after
iterator_to_array: A before, B, A after
iterator_apply: A before, B, A after
preg_replace_callback: A before, B, A after
nested array_map: A before, B, A after
