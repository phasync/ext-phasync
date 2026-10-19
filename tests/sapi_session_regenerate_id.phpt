--TEST--
sapi(): session_regenerate_id() inside a request changes only that request's id; the host is unaffected
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

$dir = sys_get_temp_dir() . '/phasync_sapi_regen_' . getmypid();
@mkdir($dir);
ini_set('session.save_path', $dir);

session_id('host-regen');
session_start();

$before = null; $after = null;
sapi(function () use (&$before, &$after) {
    session_id('regen-a');
    session_start();
    $before = session_id();
    session_regenerate_id(true);
    $after = session_id();
}, new Sink);

var_dump($before, $after, $before !== $after, session_id());

session_write_close();
array_map('unlink', glob("$dir/sess_*"));
rmdir($dir);
?>
--EXPECTF--
string(7) "regen-a"
string(%d) "%s"
bool(true)
string(10) "host-regen"
