--TEST--
sapi(): I/O in an autoloader reached through class_exists(), spl_autoload_call() and unserialize() runs inline
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
c_invoked("class_exists", function ($sleep) { spl_autoload_register(function ($c) use ($sleep) { if ($c !== 'CiAuto1') return; $sleep(); eval("class $c {}"); }); class_exists('CiAuto1'); });
c_invoked("spl_autoload_call", function ($sleep) { spl_autoload_register(function ($c) use ($sleep) { if ($c !== 'CiAuto2') return; $sleep(); }); spl_autoload_call('CiAuto2'); });
c_invoked("unserialize", function ($sleep) { spl_autoload_register(function ($c) use ($sleep) { if ($c !== 'CiAuto3') return; $sleep(); eval("class $c {}"); }); unserialize('O:7:"CiAuto3":0:{}'); });
c_invoked("static call", function ($sleep) { spl_autoload_register(function ($c) use ($sleep) { if ($c !== 'CiAuto4') return; $sleep(); eval("class $c { static function f() {} }"); }); CiAuto4::f(); });
?>
--EXPECT--
class_exists: A before, A after, B
spl_autoload_call: A before, A after, B
unserialize: A before, A after, B
static call: A before, A after, B
