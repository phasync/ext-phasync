--TEST--
sapi(): I/O in a header_register_callback() callback runs inline
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
c_invoked("header callback", function ($sleep) { header_register_callback(function () use ($sleep) { $sleep(); }); echo "x"; flush(); });
c_invoked("header callback (second)", function ($sleep) { header_register_callback(function () use ($sleep) { $sleep(); }); header_register_callback(fn () => null); header_register_callback(function () use ($sleep) { $sleep(); }); echo "x"; });
?>
--EXPECT--
header callback: A before, A after, B
header callback (second): A before, A after, B
