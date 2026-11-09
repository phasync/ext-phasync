--TEST--
memo json_decode() in object mode: get_object_vars() on a cached object (Joomla's
Registry::asArray() pattern) shares its properties table with every object handed out, each
still destroyed as if it owned it alone; a later one reaches refcount zero and frees a
still-shared, pemalloc'd block (ext-phasync#131)
--EXTENSIONS--
phasync
json
--INI--
phasync.memo_min_ns=0
phasync.memo_memory=64M
--FILE--
<?php
function asArray($data) {
    $array = [];
    if (\is_object($data)) {
        $data = \get_object_vars($data);
    }
    foreach ($data as $k => $v) {
        if (\is_object($v) || \is_array($v)) {
            $array[$k] = asArray($v);
            continue;
        }
        $array[$k] = $v;
    }
    return $array;
}

// All-scalar top level: the "deep" flag stays false, taking the shortcut that skipped
// zend_array_dup() of the properties table.
$json = '{"a":1,"b":true,"c":false,"d":null,"e":2.5,"f":"text"}';

$first  = json_decode($json);   // decoded, not stored yet
$second = json_decode($json);   // decoded again, now stored (phasync_memo_second_time())
unset($first, $second);

for ($i = 0; $i < 50; $i++) {
    $hit = json_decode($json);   // every one of these shares the cached properties table
    $arr = asArray($hit);
    unset($hit, $arr);
}

echo "ok\n";
?>
--EXPECT--
ok
