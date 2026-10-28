--TEST--
memo: on by default, with phasync.memo_memory=16M
--EXTENSIONS--
phasync
json
--FILE--
<?php
$t = json_encode(range(1, 100)); // decodes in over 500 ns, so it is stored
for ($i = 0; $i < 3; $i++) { json_decode($t, true); }
$s = phasync\ext\memo_stats();
var_dump(ini_get('phasync.memo_memory'), $s['enabled'], $s['json_hits']);
?>
--EXPECT--
string(3) "16M"
bool(true)
int(1)
