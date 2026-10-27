--TEST--
sapi() with the memcached session handler: a request waiting for a session another request of the worker holds sleeps in its coroutine, so the holder runs and the worker keeps running (#12)
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
use function phasync\ext\sapi;
ini_set('session.save_handler', 'memcached');
ini_set('session.save_path', MEMC_SERVER);
ini_set('session.use_strict_mode', '0');
ini_set('session.use_cookies', '0');
ini_set('session.cache_limiter', '');
ini_set('memcached.sess_locking', '1');
ini_set('memcached.sess_lock_wait_min', '100');      // a blocking retry sleep would stall the worker this long
ini_set('memcached.sess_lock_wait_max', '100');
ini_set('memcached.sess_lock_retries', '50');
$id = 'phasynclock' . getmypid();

$loop = new Loop;
$log = [];
$gap = 0.0;
$done = 0;
// A holds the session for a while; B, arriving meanwhile, waits for A's write.
$loop->go(function () use ($id, &$log, &$done) {
    sapi(function () use ($id, &$log) {
        session_id($id);
        session_start();
        $log[] = 'A has the session';
        usleep(200000);
        $_SESSION['by'] = 'A';
        $log[] = 'A done';
    }, new Sink);
    $done++;
});
$loop->go(function () use ($id, &$log, &$done) {
    usleep(20000);
    sapi(function () use ($id, &$log) {
        $log[] = 'B asks for the session';
        session_id($id);
        session_start();
        $log[] = 'B has the session, written by ' . ($_SESSION['by'] ?? 'nobody');
        session_destroy();
    }, new Sink);
    $done++;
});
$loop->go(function () use (&$gap, &$done) {         // the longest the worker went without running this
    for ($t = microtime(true); $done < 2; $t = $now) {
        usleep(10000);
        $gap = max($gap, ($now = microtime(true)) - $t);
    }
});
$loop->run();
echo implode("\n", $log), "\n";
echo $gap < 0.06 ? "the worker kept running\n" : sprintf("BLOCKED for %d ms\n", $gap * 1000);
CHILD);
?>
--EXPECT--
A has the session
B asks for the session
A done
B has the session, written by A
the worker kept running
