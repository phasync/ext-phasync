--TEST--
memo: a hit is an immutable value; every way of writing to it (assignment, references, by-reference foreach and functions, string appends, object properties) changes the caller's copy only
--EXTENSIONS--
phasync
json
--INI--
phasync.memo_memory=8M
--FILE--
<?php
$json = json_encode(['a' => ['b' => ['c' => 1]], 'list' => [3, 1, 2], 's' => 'str', 'o' => ['x' => 1]]);
$expect = json_decode($json, true, 64);   // native: other depth, never memoised
for ($i = 0; $i < 3; $i++) { json_decode($json, true); json_decode($json); }
$check = static fn () => json_decode($json, true) === $expect && json_decode($json) == json_decode($json, false, 64);

$a = json_decode($json, true); $a['a']['b']['c'] = 2;
$a = json_decode($json, true); $r = &$a['a']['b']; $r['c'] = 3; unset($r);
$a = json_decode($json, true); foreach ($a['list'] as &$v) { $v *= 10; } unset($v);
$a = json_decode($json, true); sort($a['list']); array_walk($a, static function (&$v) { $v = null; });
$a = json_decode($json, true); $a['s'] .= 'x'; $a['new'] = [1];
$a = json_decode($json, true); $b = $a; $b['list'][] = 4; array_pop($a['list']);
echo 'arrays: ', var_export($check(), true), "\n";

$o = json_decode($json); $o->s = 'changed'; $o->list[] = 4; $o->o->x = 2; unset($o->a);
$o = json_decode($json); foreach ($o as $k => &$v) { $v = $k; } unset($v);
$o = json_decode($json); $p = &$o->o->x; $p = 9; unset($p);
$o = json_decode($json); $arr = (array) $o; $arr['s'] = 'x'; $vars = get_object_vars($o->o); $vars['x'] = 5;
$o = json_decode($json); $o->o->y = 1; $o->o->x++;
echo 'objects: ', var_export($check(), true), "\n";

$file = tempnam(sys_get_temp_dir(), 'phasync-memo-');
file_put_contents($file, 'contents');
for ($i = 0; $i < 3; $i++) { $c = file_get_contents($file); }
$c .= ' more'; $c[0] = 'C';
$d = file_get_contents($file);
echo 'strings: ', $d, ' / ', $c, "\n";
unlink($file);
?>
--EXPECT--
arrays: true
objects: true
strings: contents / Contents more
