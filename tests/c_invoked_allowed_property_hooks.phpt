--TEST--
sapi(): I/O in a property hook suspends
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_VERSION_ID < 80400) echo 'skip property hooks need PHP 8.4'; ?>
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiP { public static $s; public int $p { get { (self::$s)(); return 1; } set { (self::$s)(); } } }
c_invoked("get hook", function ($sleep) { CiP::$s = $sleep; (new CiP)->p; });
c_invoked("set hook", function ($sleep) { CiP::$s = $sleep; $o = new CiP; $o->p = 1; });
?>
--EXPECT--
get hook: A before, B, A after
set hook: A before, B, A after
