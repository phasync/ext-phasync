--TEST--
sapi(): I/O in a constructor run by constant-expression evaluation (new in an initializer) runs inline
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiC { static $s; function __construct() { (self::$s)(); } }
function ci_default($x = new CiC) {}
c_invoked("new in initializer", function ($sleep) { CiC::$s = $sleep; ci_default(); });
c_invoked("plain new", function ($sleep) { CiC::$s = $sleep; new CiC; });
c_invoked("ReflectionClass::newInstance", function ($sleep) { CiC::$s = $sleep; (new ReflectionClass(CiC::class))->newInstance(); });
?>
--EXPECT--
new in initializer: A before, A after, B
plain new: A before, B, A after
ReflectionClass::newInstance: A before, B, A after
