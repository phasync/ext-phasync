--TEST--
run_next(): memory does not grow across many runs
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/mem.php", <<<'PHP'
<?php
function rss() { return (int) explode(' ', file_get_contents('/proc/self/statm'))[1] * 4096; }
class Big { public array $a; function __construct() { $this->a = range(1, 20000); } }
$n = count(file($argv[1])) + 1;
file_put_contents($argv[1], "$n\n", FILE_APPEND);
$keep = [new Big, str_repeat('x', 1 << 20)];
static $s = [1, 2, 3];
if ($n == 20) {
    file_put_contents($argv[2], json_encode([memory_get_usage(), rss()]));
}
if ($n < 200) {
    phasync\ext\run_next(__FILE__);
} else {
    [$m0, $r0] = json_decode(file_get_contents($argv[2]));
    $dm = memory_get_usage() - $m0;
    $dr = rss() - $r0;
    echo "runs: $n\n";
    echo "usage delta ", abs($dm) < 65536 ? "ok" : $dm, "\n";
    echo "rss delta ", $dr < 4 << 20 ? "ok" : $dr, "\n";
}
PHP);
[$out, $status] = rn_exec($d, "$d/mem.php", ["$d/state", "$d/base"]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
runs: 200
usage delta ok
rss delta ok
status: 0
