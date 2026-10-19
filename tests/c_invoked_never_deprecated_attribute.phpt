--TEST--
sapi(): I/O in the error handler of a #[\Deprecated] function runs inline
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_VERSION_ID < 80400) echo 'skip #[\Deprecated] needs PHP 8.4'; ?>
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
#[\Deprecated]
function ci_old() {}
c_invoked("deprecated function", function ($sleep) { set_error_handler(function () use ($sleep) { $sleep(); return true; }); ci_old(); });
?>
--EXPECT--
deprecated function: A before, A after, B
