--TEST--
memo: off unless phasync.memo_memory is set (the default is 0)
--EXTENSIONS--
phasync
json
--FILE--
<?php
$t = json_encode(['a' => 1]);
for ($i = 0; $i < 3; $i++) { json_decode($t, true); }
$s = phasync\ext\memo_stats();
var_dump(ini_get('phasync.memo_memory'), $s['enabled'], $s['json_hits'], $s['misses']);
?>
--EXPECT--
string(1) "0"
bool(false)
int(0)
int(0)
