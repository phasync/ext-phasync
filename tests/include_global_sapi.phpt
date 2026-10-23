--TEST--
include_global() runs in the global scope inside phasync\ext\sapi(), seeing that request's own superglobals
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;
use function phasync\ext\include_global;

$a = new Sink;
$a->info = ['method' => 'GET', 'query_string' => ''];

$seen = [];
sapi(function () use (&$seen) {
    ob_start(); // the fixture's own echo'd __FILE__/__DIR__ go to $a->out, not this test's stdout
    $seen['ret'] = include_global(__DIR__ . '/include_global_a.inc');
    ob_end_clean();
    $seen['x'] = $GLOBALS['x'];
    $seen['reader'] = include_global_reader();
}, $a);
var_dump($seen);
?>
--EXPECT--
array(3) {
  ["ret"]=>
  string(13) "return-from-a"
  ["x"]=>
  string(8) "set-by-a"
  ["reader"]=>
  string(8) "set-by-a"
}
