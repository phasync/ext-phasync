--TEST--
sapi(): ext/session's state -- session_start(), $_SESSION, session_id(), session_status() -- is each request's own; a session started inside one request never reaches the host or a sibling request
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

$dir = sys_get_temp_dir() . '/phasync_sapi_sess_' . getmypid();
@mkdir($dir);
ini_set('session.save_path', $dir);

// The host's own session, started before any sapi() call.
session_id('host-id');
session_start();
$_SESSION['host'] = true;

$a = new Sink; $b = new Sink;
$fa = new Fiber(fn() => sapi(function () {
    session_id('a-id');
    session_start();
    $_SESSION['who'] = 'a';
    Fiber::suspend();
    echo "a: ", session_id(), " ", session_status(), " ", json_encode($_SESSION), "\n";
}, $a));
$fb = new Fiber(fn() => sapi(function () {
    session_id('b-id');
    session_start();
    $_SESSION['who'] = 'b';
    Fiber::suspend();
    echo "b: ", session_id(), " ", session_status(), " ", json_encode($_SESSION), "\n";
}, $b));
$fa->start();
$fb->start();
echo "host while both suspended: ", session_id(), " ", session_status(), " ", json_encode($_SESSION), "\n";
$fa->resume();
$fb->resume();
echo $a->out, $b->out;

// Neither request's session reached the host.
echo "host after: ", session_id(), " ", session_status(), " ", json_encode($_SESSION), "\n";

session_write_close();
array_map('unlink', glob("$dir/sess_*"));
rmdir($dir);
?>
--EXPECT--
host while both suspended: host-id 2 {"host":true}
a: a-id 2 {"who":"a"}
b: b-id 2 {"who":"b"}
host after: host-id 2 {"host":true}
