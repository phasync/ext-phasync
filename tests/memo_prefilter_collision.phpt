--TEST--
memo pre-filter: texts with equal length and equal middle 64 bytes (one fingerprint) never share a result; a hashed (interned) text skips the fingerprint
--EXTENSIONS--
phasync
json
--INI--
phasync.memo_memory=64M
--FILE--
<?php
$s = static fn () => phasync\ext\memo_stats();
$mid = str_repeat('m', 100);
// equal length, equal middle; different at both ends
$a = '[1,"' . $mid . '",[2]]';
$b = '[3,"' . $mid . '",[4]]';
$c = '{"' . $mid . '":7}';
$d = '{"' . $mid . '":8}';
$ok = true;
for ($i = 0; $i < 4; $i++) {
    $ok = $ok && json_decode($a, true) === [1, $mid, [2]] && json_decode($b, true) === [3, $mid, [4]];
    $ok = $ok && json_decode($c) == (object) [$mid => 7] && json_decode($d) == (object) [$mid => 8];
}
$h = $s()['json_hits'];
echo 'json: ', var_export($ok, true), ', hits ', $h > 0 ? 'yes' : 'no', "\n";

$big = range(1, 100); // heavy enough to decode in over 500 ns, so the result is stored
$x = serialize(['k' => 1, 'v' => $big, 'z' => 'a']);
$y = serialize(['k' => 2, 'v' => $big, 'z' => 'b']);
$ok = strlen($x) === strlen($y);
for ($i = 0; $i < 4; $i++) {
    $ok = $ok && unserialize($x) === ['k' => 1, 'v' => $big, 'z' => 'a'] && unserialize($y) === ['k' => 2, 'v' => $big, 'z' => 'b'];
}
// 5 hits, not 4: $y shares $x's fingerprint, so its first sight counts as its second and stores it
echo 'unserialize: ', var_export($ok, true), ', hits ', $s()['unserialize_hits'], "\n";

// short texts: the whole text is the fingerprint
$ok = true;
for ($i = 0; $i < 3; $i++) {
    $ok = $ok && json_decode('[1]', true) === [1] && json_decode('[2]', true) === [2] && json_decode(str_repeat(' ', 1) . '[1]', true) === [1];
}
echo 'short: ', var_export($ok, true), "\n";
?>
--EXPECT--
json: true, hits yes
unserialize: true, hits 5
short: true
