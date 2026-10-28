--TEST--
sapi(): register_shutdown_function() inside a request registers for it: run in registration order (nested ones too) when $code returns, in the request (its $_SERVER, header(), output to $sapi), before its headers are sent and its session written; the worker's run only when the worker ends (#19, WordPress _wp_cron())
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

$dir = sys_get_temp_dir() . '/phasync_sapi_shutdown_' . getmypid();
@mkdir($dir);
ini_set('session.save_path', $dir);
register_shutdown_function(function () use ($dir) {
    echo "worker shutdown\n";
    array_map('unlink', glob("$dir/*"));
    rmdir($dir);
});

$s = new Sink;
$s->info = ['method' => 'GET', 'server' => ['REQUEST_URI' => '/page?x=1']];
(new Scope)->run(fn() => sapi(function () {
    session_id('sd-id');
    session_start();
    register_shutdown_function(function (string $arg) {
        header('X-Shutdown: 1');
        echo "first $arg ", $_SERVER['REQUEST_URI'], "\n";
        register_shutdown_function(function () {
            echo "nested\n";
            $_SESSION['by'] = 'nested';
        });
    }, 'arg');
    register_shutdown_function(function () { echo "second\n"; });
}, $s));
echo "after sapi\n";
echo $s->out;
var_dump(in_array('X-Shutdown: 1', $s->headers[2]));
$s2 = new Sink;
(new Scope)->run(fn() => sapi(function () {
    session_id('sd-id');
    session_start();
    echo json_encode($_SESSION), "\n";
}, $s2));
echo $s2->out;
echo "done\n";
?>
--EXPECT--
after sapi
first arg /page?x=1
second
nested
bool(true)
{"by":"nested"}
done
worker shutdown
