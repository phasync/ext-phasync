--TEST--
sapi(): I/O in a stream wrapper method other than stream_open/read/write runs inline (#107)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiW { public $context; static $s; function stream_open($p, $m, $o, &$op) { return true; } function stream_write($d) { return strlen($d); } function stream_close() { (self::$s)(); } }
c_invoked("stream wrapper", function ($sleep) { CiW::$s = $sleep; stream_wrapper_register("ciw", "CiW"); $f = fopen("ciw://x", "w"); fclose($f); });
?>
--EXPECT--
stream wrapper: A before, A after, B
