--TEST--
memo: a json_decode() or unserialize() faster than phasync.memo_min_ns is not stored; slower ones are
--EXTENSIONS--
phasync
json
--INI--
phasync.memo_min_ns=1000000000
--FILE--
<?php
// every decode here takes far less than a second, so nothing is stored
$j = json_encode(range(1, 1000));
$u = serialize(range(1, 1000));
for ($i = 0; $i < 4; $i++) { json_decode($j, true); unserialize($u); }
$s = phasync\ext\memo_stats();
var_dump(ini_get('phasync.memo_min_ns'), $s['json_hits'], $s['unserialize_hits']);
?>
--EXPECT--
string(10) "1000000000"
int(0)
int(0)
