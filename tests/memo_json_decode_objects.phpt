--TEST--
memo json_decode($json) (objects): a JSON text decoded before comes back as a new clone each time -- new stdClass instances, everything else shared copy-on-write; writes at any depth never reach the cache; {} and {"0":...} stay objects
--EXTENSIONS--
phasync
json
--INI--
phasync.memo_min_ns=0
phasync.memo_memory=64M
--FILE--
<?php
$json = json_encode([
    'a' => ['b' => ['c' => 1]],
    'list' => [['x' => 1], ['y' => 2]],
    'plain' => ['k' => 'v', 'n' => [1, 2, 3]],
    'pad' => str_repeat('p', 1100),
]);
$json = substr($json, 0, -1) . ',"empty":{},"numeric":{"0":"a","1":"b"}}';
$expected = serialize(json_decode($json));    // the reference: decoded, the text's first sight
$hits = static fn () => phasync\ext\memo_stats()['json_hits'];

$h0 = $hits();
$r1 = json_decode($json);
$r2 = json_decode($json);
$r3 = json_decode($json);
echo 'equal to a native decode: ', var_export(serialize($r1) === $expected && serialize($r3) === $expected, true), ', hits: ', $hits() - $h0, "\n";
echo 'new instances each time: ', var_export($r2 !== $r3 && $r2->a !== $r3->a && $r2->list[0] !== $r3->list[0], true), "\n";
echo '{} and {"0":..} are objects: ', var_export($r3->empty instanceof stdClass && $r3->numeric instanceof stdClass && $r3->numeric->{'1'} === 'b', true), "\n";

$r3->a->b->c = 'changed';           // a nested object
$r3->list[0]->x = 'changed';        // an object inside an array
$r3->plain->n[] = 4;                // an array inside an object
$r3->added = true;                  // a new property
unset($r3->pad);
$r3->empty->now = 'set';
echo 'cache untouched by writes: ', var_export(serialize(json_decode($json)) === $expected, true), "\n";
echo 'other clone untouched: ', var_export($r2->a->b->c === 1 && $r2->list[0]->x === 1 && count($r2->plain->n) === 3 && !isset($r2->added) && isset($r2->pad), true), "\n";
?>
--EXPECT--
equal to a native decode: true, hits: 2
new instances each time: true
{} and {"0":..} are objects: true
cache untouched by writes: true
other clone untouched: true
