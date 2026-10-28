--TEST--
memo: entries evicted while the request still holds their values (arrays, strings, object-mode results sharing property tables, keys) stay valid until the request ends
--EXTENSIONS--
phasync
json
--INI--
phasync.memo_memory=128K
--FILE--
<?php
$file = tempnam(sys_get_temp_dir(), 'phasync-memo-');
file_put_contents($file, str_repeat('abc', 1000));
for ($i = 0; $i < 3; $i++) { $contents = file_get_contents($file); }
$held = [];
for ($i = 0; $i < 300; $i++) {
    $t = json_encode(['i' => $i, "key$i" => "value$i", 'list' => range(1, 20), 'o' => ['p' => "s$i"]]);
    for ($k = 0; $k < 3; $k++) { $a = json_decode($t, true); $o = json_decode($t); }
    $held[] = [$a, $o, array_keys($a)[1]];   // hits: immutable values, an object sharing its tables
}
$s = phasync\ext\memo_stats();
echo 'evicted: ', $s['evicted'] > 100 ? 'many' : $s['evicted'], "\n";
$ok = $contents === str_repeat('abc', 1000);
foreach ($held as $i => [$a, $o, $key]) {
    $ok = $ok && $a['i'] === $i && $a["key$i"] === "value$i" && $key === "key$i" && $a['list'][19] === 20
        && $o->o->p === "s$i" && $o->{"key$i"} === "value$i" && count($o->list) === 20;
}
echo 'held values intact: ', var_export($ok, true), "\n";
unset($held);
unlink($file);
?>
--EXPECT--
evicted: many
held values intact: true
