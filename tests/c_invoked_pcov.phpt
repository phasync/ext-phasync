--TEST--
sapi(): with an execute_ex override loaded (pcov), ordinary nested calls still suspend and the never-list still runs inline (#2)
--EXTENSIONS--
phasync
pcov
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';

/* Ordinary PHP-to-PHP calls: pcov forces every call to recurse through execute_ex,
 * so these get ZEND_CALL_TOP too, with arguments shaped like an error handler's or
 * a pcntl signal handler's; they must still suspend. */
class CiPcov { static $sleep;
  function __construct(int $signo, array $info) { (self::$sleep)(); }
  function m(int $no, string $str, string $file, int $line) { (self::$sleep)(); } }
function ci_plain2(int $signo, array $info) { (CiPcov::$sleep)(); }
function ci_plain4(int $no, string $str, string $file, int $line) { (CiPcov::$sleep)(); }

c_invoked("constructor via new", function ($sleep) { CiPcov::$sleep = $sleep; new CiPcov(1, ['signo' => 1]); });
c_invoked("method call", function ($sleep) { CiPcov::$sleep = fn () => null; $o = new CiPcov(0, []); CiPcov::$sleep = $sleep; $o->m(1, "x", "y", 2); });
c_invoked("closure call", function ($sleep) { CiPcov::$sleep = $sleep; (function (int $signo, array $info) { (CiPcov::$sleep)(); })(1, ['signo' => 1]); });
c_invoked("function (int, array)", function ($sleep) { CiPcov::$sleep = $sleep; ci_plain2(1, ['signo' => 1]); });
c_invoked("function (int, string, string, int)", function ($sleep) { CiPcov::$sleep = $sleep; ci_plain4(1, "x", "y", 2); });

/* A few never-list cases (c_invoked_usort.phpt, c_invoked_never_magic_property.phpt,
 * c_invoked_never_error_handler.phpt): still run inline with pcov loaded. */
c_invoked("usort comparator", function ($sleep) { $a = [2, 1]; usort($a, function ($x, $y) use ($sleep) { $sleep(); return $x <=> $y; }); });
class CiPcovGet { function __construct(public Closure $s) {} function __get($n) { ($this->s)(); return 1; } }
c_invoked("__get", function ($sleep) { $o = new CiPcovGet($sleep); $o->x; });
c_invoked("error handler", function ($sleep) { set_error_handler(function () use ($sleep) { $sleep(); return true; }); $x = $undefined; });
?>
--EXPECT--
constructor via new: A before, B, A after
method call: A before, B, A after
closure call: A before, B, A after
function (int, array): A before, B, A after
function (int, string, string, int): A before, B, A after
usort comparator: A before, A after, B
__get: A before, A after, B
error handler: A before, A after, B
