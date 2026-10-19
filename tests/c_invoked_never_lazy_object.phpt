--TEST--
sapi(): I/O in a lazy object initializer runs inline
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_VERSION_ID < 80400) echo 'skip lazy objects need PHP 8.4'; ?>
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiL { public $v = 1; }
c_invoked("lazy ghost", function ($sleep) { $o = (new ReflectionClass(CiL::class))->newLazyGhost(function ($o) use ($sleep) { $sleep(); }); $o->v; });
c_invoked("lazy proxy", function ($sleep) { $o = (new ReflectionClass(CiL::class))->newLazyProxy(function ($o) use ($sleep) { $sleep(); return new CiL; }); $o->v; });
?>
--EXPECT--
lazy ghost: A before, A after, B
lazy proxy: A before, A after, B
