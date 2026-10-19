--TEST--
sapi(): I/O in a preg_replace_callback()/preg_replace_callback_array() callback runs inline (ext/pcre's mdata_used is process-wide, not per call)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
c_invoked("preg_replace_callback", function ($sleep) {
    preg_replace_callback('/a/', function ($m) use ($sleep) { $sleep(); return 'b'; }, 'aaa');
});
c_invoked("preg_replace_callback_array", function ($sleep) {
    preg_replace_callback_array(['/a/' => function ($m) use ($sleep) { $sleep(); return 'b'; }], 'aaa');
});
?>
--EXPECT--
preg_replace_callback: A before, A after, B
preg_replace_callback_array: A before, A after, B
