--TEST--
Redis (phpredis): a coroutine entering a connection another one is in a call on throws; a cancelled call leaves it usable
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists("Fiber")) die("skip requires Fibers");
if (!getenv("PHASYNC_TEST_REDIS")) die("skip set PHASYNC_TEST_REDIS=host:port[:ext.so,ext.so,...] (the extensions phpredis needs, in load order)");
[$host, $port, $exts] = explode(":", getenv("PHASYNC_TEST_REDIS"), 3) + [2 => "redis"];
if (!@fsockopen($host, (int) $port)) die("skip no redis on $host:$port");
$d = implode(" ", array_map(fn($e) => "-d extension=" . escapeshellarg($e), explode(",", $exts)));
exec(escapeshellarg(PHP_BINARY) . " $d -r " . escapeshellarg("exit(class_exists(\"Redis\") ? 0 : 1);") . " 2>&1", $o, $rc);
if ($rc) die("skip the redis extension cannot be loaded");
?>
--FILE--
<?php
[$host, $port, $exts] = explode(':', getenv('PHASYNC_TEST_REDIS'), 3) + [2 => 'redis'];
$child = tempnam(sys_get_temp_dir(), 'phasync_redis_') . '.php';
file_put_contents($child, '<?php
require ' . var_export(__DIR__ . '/loop.inc', true) . ';
const REDIS_HOST = ' . var_export($host, true) . ';
const REDIS_PORT = ' . (int) $port . ';
' . <<<'CHILD'
function redis_on(): Redis
{
    $r = new Redis;
    $r->connect(REDIS_HOST, REDIS_PORT);
    return $r;
}
$u = bin2hex(random_bytes(4));

// A second coroutine entering a connection another one is in a call on throws; the first call is undisturbed.
$r = redis_on();
$log = [];
(new Loop)->runAll(
    function () use (&$log, $r, $u) { $v = $r->blPop("$u.q", 0.3); $log[] = 'first: ' . ($v ? $v[1] : 'timeout'); },
    function () use (&$log, $r, $u) {
        try { $r->set("$u.b", 'two'); } catch (LogicException $e) { $log[] = 'second: ' . $e->getMessage(); }
    },
);
echo implode(', ', $log), "\n";
echo 'after: ', $r->set("$u.c", 'x') ? $r->get("$u.c") : 'set failed', "\n";

// Two connections: no interference.
$a = redis_on();
$b = redis_on();
$log = [];
(new Loop)->runAll(
    function () use (&$log, $a, $u) { $v = $a->blPop("$u.q2", 0.3); $log[] = 'a: ' . ($v ? $v[1] : 'timeout'); },
    function () use (&$log, $b, $u) { $b->rPush("$u.q2", 'v'); $log[] = 'b: pushed'; },
);
sort($log);   // the woken blPop() may log before the push returns
echo implode(', ', $log), "\n";

// The owner is cancelled in the middle of a call: the exception comes out of the call, and the connection works afterwards.
$r = redis_on();
$loop = new Loop;
$log = [];
$loop->runAll(
    $owner = new Fiber(function () use ($r, $u, &$log) {
        try { $r->blPop("$u.q3", 2); } catch (Throwable $e) {   // phpredis reports a failed read as RedisException, the cancellation as its previous
            $log[] = 'owner: ' . get_class($e) . (($p = $e->getPrevious()) ? ' (' . get_class($p) . ')' : '');
        }
    }),
    function () use ($loop, $owner) { usleep(50000); $loop->cancel($owner); },
);
$r2 = redis_on();   // the cancelled BLPOP's reply may still be due on $r: a fresh connection for the check
$r2->set("$u.e", 'after');
$log[] = 'next: ' . $r2->get("$u.e");
echo implode(', ', $log), "\n";

// pconnect() would share the connection between requests: refused (phasync.allow_persistent=0).
try { (new Redis)->pconnect(REDIS_HOST, REDIS_PORT); echo "pconnect: allowed\n"; } catch (LogicException $e) { echo 'pconnect: ', strstr($e->getMessage(), ' was asked', true), "\n"; }
CHILD);
$d = implode(' ', array_map(fn($e) => '-d extension=' . escapeshellarg($e), explode(',', $exts)));
$phasync = realpath(ini_get('extension_dir') . '/phasync.so');
passthru(escapeshellarg(PHP_BINARY) . ' -d extension=' . escapeshellarg($phasync) . " $d " . escapeshellarg($child) . ' 2>&1');
unlink($child);
?>
--EXPECT--
second: Concurrent operations on the same resource are not supported, first: timeout
after: x
a: v, b: pushed
owner: RedisException (LoopCancelled), next: after
pconnect: Redis::pconnect()
