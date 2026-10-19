--TEST--
Memcached calls (php-memcached) suspend the coroutine instead of blocking the process, with the same results
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists("Fiber")) die("skip requires Fibers");
if (!function_exists("proc_open")) die("skip requires proc_open");
if (!getenv("PHASYNC_TEST_MEMCACHED")) die("skip set PHASYNC_TEST_MEMCACHED=host:port[:/path/to/memcached.so]");
[$host, $port, $so] = explode(":", getenv("PHASYNC_TEST_MEMCACHED")) + [2 => "memcached"];
if (!@fsockopen($host, (int) $port)) die("skip no memcached on $host:$port");
exec(escapeshellarg(PHP_BINARY) . " -d extension=" . escapeshellarg($so) . " -r " . escapeshellarg("exit(class_exists(\"Memcached\") ? 0 : 1);") . " 2>&1", $o, $rc);
if ($rc) die("skip the memcached extension cannot be loaded");
?>
--FILE--
<?php
require __DIR__ . '/memcached.inc';
// Every reply is delayed by a proxy, so overlapping calls show on the clock.
memc_child(<<<'CHILD'
$delay = 200;
[$p1] = memc_proxy($delay);
[$p2] = memc_proxy($delay);
$u = bin2hex(random_bytes(4));

// One handle per coroutine: two coroutines' calls overlap, and the results are those of a call outside any coroutine.
function work(Memcached $m, string $k): array
{
    $m->set($k, 'v');
    return [$m->get($k), $m->getResultCode(), $m->get($k . '.missing'), $m->getResultCode(), $m->add($k, 'w'), $m->getResultCode()];
}
$t = microtime(true);
$plain = work(memc_on($p1), "$u.plain");
$one = microtime(true) - $t;
$r = [];
$t = microtime(true);
$loop = new Loop;
$loop->runAll(function () use (&$r, $p1, $u) { $r['a'] = work(memc_on($p1), "$u.a"); }, function () use (&$r, $p1, $u) { $r['b'] = work(memc_on($p1), "$u.b"); });
$both = microtime(true) - $t;
printf("results equal: %s\n", var_export($r == ['a' => $plain, 'b' => $plain], true));
printf("overlapped: %s\n", var_export($both < $one * 1.5, true));
printf("parked: %s\n", var_export($loop->parks >= 4, true));

// getMulti across two servers waits on both sockets at once.
$direct = memc_on(...[(int) explode(':', MEMC_SERVER)[1]]);
$keys = [];
for ($i = 0; $i < 20; $i++) {
    $keys[] = "$u.m$i";
    $direct->set("$u.m$i", "val$i");
}
$multi = fn() => memc_on($p1, $p2)->getMulti(array_merge($keys, ["$u.nokey"]));
$t = microtime(true);
$plain = $multi();
$one = microtime(true) - $t;
$r = [];
$t = microtime(true);
$loop = new Loop;
$loop->runAll(function () use (&$r, $multi) { $r[] = $multi(); }, function () use (&$r, $multi) { $r[] = $multi(); });
$both = microtime(true) - $t;
printf("getMulti: %d keys, equal: %s, overlapped: %s\n", count($plain),
    var_export($r == [$plain, $plain], true), var_export($both < $one * 1.5, true));

// The rest of the API answers the same inside a coroutine as outside.
function api(Memcached $m, string $p): array
{
    $o = [];
    $m->set("$p.n", 5);
    $o[] = [$m->increment("$p.n", 3), $m->decrement("$p.n"), $m->getResultCode()];
    $o[] = [$m->append("$p.s", 'x'), $m->getResultCode(), $m->set("$p.s", 'b'), $m->append("$p.s", 'c'), $m->prepend("$p.s", 'a'), $m->get("$p.s")];
    $o[] = [$m->replace("$p.none", 1), $m->getResultCode(), $m->getResultMessage()];
    $g = $m->get("$p.s", null, Memcached::GET_EXTENDED);
    $o[] = [$m->cas($g['cas'], "$p.s", 'new'), $m->cas($g['cas'], "$p.s", 'again'), $m->getResultCode(), $m->get("$p.s")];
    $o[] = [$m->touch("$p.s", 100), $m->getDelayed(["$p.s", "$p.n"]), count($m->fetchAll()), $m->delete("$p.s"), $m->delete("$p.s"), $m->getResultCode()];
    $o[] = [$m->setMulti(["$p.1" => 1, "$p.2" => 2]), $m->getMulti(["$p.1", "$p.2", "$p.3"]), $m->deleteMulti(["$p.1", "$p.2"])];
    $o[] = [count($m->getVersion()), count($m->getStats()), $m->quit()];
    return $o;
}
[$fast] = memc_proxy(20);
$plain = api(memc_on($fast), "$u.api");
$r = null;
(new Loop)->runAll(function () use (&$r, $fast, $u) { $r = api(memc_on($fast), "$u.api"); });
echo 'api equal: ', var_export($r === $plain, true), "\n";
CHILD);
?>
--EXPECT--
results equal: true
overlapped: true
parked: true
getMulti: 20 keys, equal: true, overlapped: true
api equal: true
