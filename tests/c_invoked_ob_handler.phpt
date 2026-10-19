--TEST--
sapi(): I/O in an output buffer handler runs inline
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
c_invoked("output handler", function ($sleep) { ob_start(function ($b) use ($sleep) { $sleep(); return $b; }); echo "x"; ob_end_flush(); });
?>
--EXPECT--
output handler: A before, A after, B
