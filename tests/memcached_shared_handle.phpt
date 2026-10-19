--TEST--
Memcached: a coroutine entering a connection another one is in a call on throws; a cancelled call leaves it usable
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
memc_child(<<<'CHILD'
[$p] = memc_proxy(100);
$u = bin2hex(random_bytes(4));

// A second coroutine entering a connection another one is in a call on throws; the first call is undisturbed.
$run = function (Memcached $x, Memcached $y) use ($u) {
    $log = [];
    (new Loop)->runAll(
        function () use (&$log, $x, $u) { $x->set("$u.a", 'one'); $log[] = 'first: ' . $x->get("$u.a"); },
        function () use (&$log, $y, $u) {
            try { $y->set("$u.b", 'two'); } catch (LogicException $e) { $log[] = 'second: ' . $e->getMessage(); }
        },
    );
    return implode(', ', $log);
};
echo 'shared object: ', $run($m = memc_on($p), $m), "\n";

// Two objects, one persistent connection: the same.
$a = new Memcached('shared-' . $u);
$b = new Memcached('shared-' . $u);
if (!$a->getServerList()) {
    $a->setOption(Memcached::OPT_COMPRESSION, false);
    $a->addServer('127.0.0.1', $p);
}
echo 'persistent id: ', $run($a, $b), "\n";

// One after the other, coroutines share it; a subclass's inherited methods too.
class SubMemcached extends Memcached {}
$m = new SubMemcached;
$m->setOption(Memcached::OPT_COMPRESSION, false);
$m->addServer('127.0.0.1', $p);
$log = [];
(new Loop)->runAll(function () use ($m, $u, &$log) {
    $m->set("$u.c", 'x');
    $f = new Fiber(function () use ($m, $u, &$log) { $log[] = 'subclass: ' . $m->get("$u.c"); });
    $f->start();
    while (!$f->isTerminated()) { usleep(10000); }
});
echo implode(', ', $log), "\n";

// The owner is cancelled in the middle of a call: the exception comes out of the call, and the connection works afterwards.
$m = memc_on($p);
$loop = new Loop;
$log = [];
$loop->runAll(
    $owner = new Fiber(function () use ($m, $u, &$log) {
        try { $m->get("$u.c"); } catch (LoopCancelled) { $log[] = 'owner: cancelled'; }
    }),
    function () use ($loop, $owner) { usleep(50000); $loop->cancel($owner); },
);
$m->set("$u.e", 'after');
$log[] = 'next: ' . $m->get("$u.e");
echo implode(', ', $log), "\n";
CHILD);
?>
--EXPECT--
shared object: second: Concurrent operations on the same resource are not supported, first: one
persistent id: second: Concurrent operations on the same resource are not supported, first: one
subclass: x
owner: cancelled, next: after
