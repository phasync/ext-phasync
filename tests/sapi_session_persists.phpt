--TEST--
sapi(): a request's session is written and closed, as RSHUTDOWN would, when it returns; a later request with the same id reads it back
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

$dir = sys_get_temp_dir() . '/phasync_sapi_sessw_' . getmypid();
@mkdir($dir);
ini_set('session.save_path', $dir);

sapi(function () {
    session_id('writer-id');
    session_start();
    $_SESSION['n'] = 1;
}, new Sink);

$seen = null;
sapi(function () use (&$seen) {
    session_id('writer-id');
    session_start();
    $seen = [session_id(), $_SESSION];
}, new Sink);
var_dump($seen);

// session_write_close() inside the request also releases it early.
sapi(function () {
    session_id('early-id');
    session_start();
    $_SESSION['early'] = true;
    session_write_close();
}, new Sink);
$seen = null;
sapi(function () use (&$seen) {
    session_id('early-id');
    session_start();
    $seen = $_SESSION;
}, new Sink);
var_dump($seen);

array_map('unlink', glob("$dir/sess_*"));
rmdir($dir);
?>
--EXPECT--
array(2) {
  [0]=>
  string(9) "writer-id"
  [1]=>
  array(1) {
    ["n"]=>
    int(1)
  }
}
array(1) {
  ["early"]=>
  bool(true)
}
