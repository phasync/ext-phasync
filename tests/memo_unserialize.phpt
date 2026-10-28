--TEST--
memo unserialize($data): a text unserialized before (seen twice) whose result holds no references and no objects but stdClass comes back cached -- arrays copy-on-write, stdClass instances cloned; one holding other objects, one object twice, references, or given options other than allowed_classes is never remembered
--EXTENSIONS--
phasync
--INI--
phasync.memo_min_ns=0
phasync.memo_memory=64M
--FILE--
<?php
class Woken { public static int $wakeups = 0; public $v = 1; public function __wakeup() { self::$wakeups++; } }
$hits  = static fn () => phasync\ext\memo_stats()['unserialize_hits'];
$plain = serialize(['siteurl' => 'http://example.org', 'active_plugins' => ['a/a.php', 'b/b.php'], 'n' => 42, 'f' => 1.5, 'nested' => ['x' => [true, null]]]);

$h = $hits();
$a = unserialize($plain); $b = unserialize($plain); $c = unserialize($plain);
echo 'plain: equal ', var_export($a === $b && $b === $c && $c === unserialize($plain), true), ', hits ', $hits() - $h, "\n";
$c['nested']['x'][] = 'changed';
echo 'cache unchanged by a write: ', var_export(unserialize($plain) === $a, true), "\n";

$h = $hits();
$obj = serialize(['o' => new Woken()]);
unserialize($obj); unserialize($obj); unserialize($obj);
echo 'other classes: hits ', $hits() - $h, ', __wakeup ran ', Woken::$wakeups, " times\n";

$std = new stdClass(); $std->name = 'opt'; $std->list = [1, 2]; $std->inner = new stdClass(); $std->inner->deep = 'x';
$sstd = serialize(['cfg' => $std, 'n' => 1]);
$h = $hits();
$s1 = unserialize($sstd); $s2 = unserialize($sstd); $s3 = unserialize($sstd);
echo 'stdClass: equal ', var_export($s1 == $s3 && $s3['cfg']->inner->deep === 'x', true), ', new instances ', var_export($s2['cfg'] !== $s3['cfg'] && $s2['cfg']->inner !== $s3['cfg']->inner, true), ', hits ', $hits() - $h, "\n";
$s3['cfg']->inner->deep = 'changed'; $s3['cfg']->list[] = 3;
echo 'stdClass cache unchanged by writes: ', var_export(unserialize($sstd)['cfg']->inner->deep === 'x' && count($s2['cfg']->list) === 2, true), "\n";

$one = new stdClass(); $one->v = 1;
$twice = serialize(['a' => $one, 'b' => $one]);   // one object, two places (r: back-reference)
$h = $hits();
$t = unserialize($twice); unserialize($twice); $t = unserialize($twice);
echo 'one object twice: hits ', $hits() - $h, ', still one object ', var_export($t['a'] === $t['b'], true), "\n";

$x = ['v' => 1]; $x['r'] = &$x['v'];
$ref = serialize($x);
$h = $hits();
$r = unserialize($ref); unserialize($ref); unserialize($ref);
echo 'references: hits ', $hits() - $h, "\n";

$h = $hits();
unserialize($plain, ['max_depth' => 100]); unserialize($plain, ['max_depth' => 100]); unserialize($plain, ['max_depth' => 100]);
echo 'max_depth given: hits ', $hits() - $h, "\n";
$h = $hits();
unserialize($plain, ['allowed_classes' => false]);
echo 'allowed_classes given: hits ', $hits() - $h, "\n";
?>
--EXPECT--
plain: equal true, hits 2
cache unchanged by a write: true
other classes: hits 0, __wakeup ran 3 times
stdClass: equal true, new instances true, hits 1
stdClass cache unchanged by writes: true
one object twice: hits 0, still one object true
references: hits 0
max_depth given: hits 0
allowed_classes given: hits 1
