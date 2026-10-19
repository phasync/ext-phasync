--TEST--
sapi(): I/O in an autoloader (PHP invoked from C) runs inline, never suspending the request mid-autoload
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
c_invoked("autoloader", function ($sleep) { spl_autoload_register(function ($c) use ($sleep) { $sleep(); eval("class $c {}"); }); new CiAuto; });
?>
--EXPECT--
autoloader: A before, A after, B
