--TEST--
sapi(): a session started inside a Scope other than the root survives the request's fiber suspending and resuming; reading $_SESSION afterwards does not crash the worker (swerve#108)
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
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$dir = sys_get_temp_dir() . '/phasync_sapi_sess108_' . getmypid();
@mkdir($dir);
ini_set('session.save_path', $dir);

// Swerve wraps every request's sapi() call in a Scope of its own (HttpConnection::application()):
// (new Scope())->run(fn () => phasync::run($this->runRequest(...))). ext-scope's own session
// isolation (scope.session=1, the default) and sapi()'s own then both try to carry $_SESSION
// across the fiber switch a wait causes, and stomp on each other.
$sink = new Sink;
$f    = new Fiber(function () use ($sink) {
    (new Scope())->run(function () use ($sink) {
        sapi(function () {
            session_start();
            $_SESSION['a'] = 1;
            Fiber::suspend();   // a suspension within the request's own fiber, as phasync::sleep() causes
            echo json_encode([isset($_SESSION), $_SESSION['a'], session_status()]);
        }, $sink);
    });
});
$f->start();
$f->resume();
echo $sink->out, "\n";

array_map('unlink', glob("$dir/sess_*"));
rmdir($dir);
?>
--EXPECT--
[true,1,2]
