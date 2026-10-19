--TEST--
sapi(): I/O in a destructor run at free suspends (PHP 8.4+: 8.3 forbids a fiber switch in a destructor)
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_VERSION_ID < 80400) die("skip PHP 8.3 blocks fiber switches in destructors");
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiD { function __construct(public Closure $s) {} function __destruct() { ($this->s)(); } }
c_invoked("destructor", function ($sleep) { $d = new CiD($sleep); unset($d); });
?>
--EXPECT--
destructor: A before, B, A after
