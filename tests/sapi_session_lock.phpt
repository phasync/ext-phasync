--TEST--
sapi() inside manage(): two concurrent requests on the same session id take turns on its flock without blocking the worker, as under php-fpm
--EXTENSIONS--
phasync
session
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--INI--
session.use_strict_mode=0
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
require __DIR__ . '/loop.inc';
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$dir = sys_get_temp_dir() . '/phasync_sapi_lock_' . getmypid();
@mkdir($dir);
ini_set('session.save_path', $dir);

$loop = new Loop;
$log = [];
$ticks = 0;
$done = 0;
// A holds the session for a while; B, arriving meanwhile, waits for A's write.
$loop->go(function () use ($loop, &$log, &$done) {
    sapi(function () use ($loop, &$log) {
        session_id('sharedlockid');
        session_start();
        $log[] = 'A has the session';
        for ($i = 0; $i < 20; $i++) usleep(10000);
        $_SESSION['by'] = 'A';
        $log[] = 'A done';
    }, new Sink);
    $done++;
});
$loop->go(function () use ($loop, &$log, &$done) {
    usleep(20000);
    sapi(function () use (&$log) {
        $log[] = 'B asks for the session';
        session_id('sharedlockid');
        session_start();
        $log[] = 'B has the session, written by ' . ($_SESSION['by'] ?? 'nobody');
    }, new Sink);
    $done++;
});
$loop->go(function () use ($loop, &$ticks, &$done) {
    while ($done < 2) { usleep(10000); $ticks++; }
});
$loop->manage(fn() => $loop->run());
echo implode("\n", $log), "\n";
echo $ticks >= 10 ? "the worker kept running\n" : "BLOCKED (ticks=$ticks)\n";

// Different ids: B does not wait for A's lock at all, even though A holds
// its own session the whole time.
$log2 = [];
$loop->go(function () use ($loop, &$log2) {
    sapi(function () use ($loop, &$log2) {
        session_id('diff-a');
        session_start();
        $log2[] = 'A has its own session';
        for ($i = 0; $i < 20; $i++) usleep(10000);
        $log2[] = 'A done';
    }, new Sink);
});
$loop->go(function () use ($loop, &$log2) {
    usleep(5000);
    sapi(function () use (&$log2) {
        session_id('diff-b');
        session_start();
        $log2[] = 'B has its own session, not waiting for A';
    }, new Sink);
});
$loop->manage(fn() => $loop->run());
echo implode("\n", $log2), "\n";

array_map('unlink', glob("$dir/sess_*"));
rmdir($dir);
?>
--EXPECT--
A has the session
B asks for the session
A done
B has the session, written by A
the worker kept running
A has its own session
B has its own session, not waiting for A
A done
