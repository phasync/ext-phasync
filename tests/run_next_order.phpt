--TEST--
run_next(): a lifecycle ends in PHP's order (shutdown functions, destructors, output buffers) before the next one starts, wherever run_next() was called
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", <<<'PHP'
<?php
class D { function __construct(public string $n) {} function __destruct() { echo "destruct {$this->n}\n"; } }
$state = __DIR__ . '/state';
$n = (int) @file_get_contents($state) + 1;
file_put_contents($state, $n);
$where = $argv[1];
$next = fn() => $n < 2 && phasync\ext\run_next(__FILE__);
echo "start $n, time ", ($_SERVER['REQUEST_TIME_FLOAT'] > (float) @file_get_contents(__DIR__ . '/time') ? 'fresh' : 'stale'), "\n";
file_put_contents(__DIR__ . '/time', $_SERVER['REQUEST_TIME_FLOAT']);
register_shutdown_function(function () use ($n, $where, $next) {
    echo "shutdown $n\n";
    register_shutdown_function(fn() => print("nested shutdown $n\n"));
    if ($where === 'shutdown') $next();
});
$GLOBALS['d'] = new D("global $n");
$fiber = new Fiber(function () use ($n) { try { Fiber::suspend(); } finally { echo "fiber finally $n\n"; } });
$fiber->start();
set_exception_handler(fn() => print("leaked exception handler\n"));
ob_start(function ($b) use ($n, $where, $next) {
    if ($where === 'ob') $next();
    return $b . "ob $n\n";
});
if ($where === 'destructor') $dd = new class($next) { function __construct(public $f) {} function __destruct() { ($this->f)(); } };
if ($where === 'main') $next();
echo "end $n\n";
PHP);
foreach (['main', 'shutdown', 'destructor', 'ob'] as $where) {
    @unlink("$d/state");
    @unlink("$d/time");
    [$out, $status] = rn_exec($d, "$d/main.php", [$where]);
    echo "== $where\n", $out, "status: $status\n";
}
rn_clean($d);
?>
--EXPECT--
== main
start 1, time fresh
end 1
shutdown 1
nested shutdown 1
destruct global 1
fiber finally 1
ob 1
start 2, time fresh
end 2
shutdown 2
nested shutdown 2
destruct global 2
fiber finally 2
ob 2
status: 0
== shutdown
start 1, time fresh
end 1
shutdown 1
nested shutdown 1
destruct global 1
fiber finally 1
ob 1
start 2, time fresh
end 2
shutdown 2
nested shutdown 2
destruct global 2
fiber finally 2
ob 2
status: 0
== destructor
start 1, time fresh
end 1
shutdown 1
nested shutdown 1
destruct global 1
fiber finally 1
ob 1
start 2, time fresh
end 2
shutdown 2
nested shutdown 2
destruct global 2
fiber finally 2
ob 2
status: 0
== ob
start 1, time fresh
end 1
shutdown 1
nested shutdown 1
destruct global 1
fiber finally 1
ob 1
start 2, time fresh
end 2
shutdown 2
nested shutdown 2
destruct global 2
fiber finally 2
ob 2
status: 0
