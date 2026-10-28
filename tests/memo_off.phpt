--TEST--
memo: phasync.memo_memory=0 turns it off
--EXTENSIONS--
phasync
json
--INI--
phasync.memo_memory=0
--FILE--
<?php
$t = json_encode(range(1, 100));
for ($i = 0; $i < 3; $i++) { json_decode($t, true); }
$s = phasync\ext\memo_stats();
var_dump(ini_get('phasync.memo_memory'), $s['enabled'], $s['json_hits'], $s['misses']);
?>
--EXPECT--
string(1) "0"
bool(false)
int(0)
int(0)
