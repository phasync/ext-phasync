--TEST--
sapi(): a client gone (write() returning false, or connection_aborted() saying so) sets connection_aborted() and discards later output; the code is not stopped
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

// write() false: aborted, later output discarded, the code runs on.
$s = new Sink; $s->gone = true;
$seen = null;
sapi(function () use (&$seen) {
    echo "one";
    echo "two";
    $seen = 'ran on';
}, $s);
var_dump($s->out, $seen);

// With ignore_user_abort(true) the code runs on, without output.
$s = new Sink; $s->gone = true;
$log = [];
sapi(function () use (&$log) {
    ignore_user_abort(true);
    echo "one";
    $log[] = connection_aborted();
    echo "two";
    $log[] = 'still running';
}, $s);
var_dump($s->out, $log);
var_dump(ignore_user_abort(), connection_status());   // the worker's own

// A server that knows the client left before any write fails says so.
$s = new class extends Sink { public function connection_aborted(): bool { return true; } };
$log = [];
sapi(function () use (&$log) { $log = [connection_aborted(), connection_status()]; }, $s);
var_dump($log);
?>
--EXPECT--
string(3) "one"
string(6) "ran on"
string(3) "one"
array(2) {
  [0]=>
  int(1)
  [1]=>
  string(13) "still running"
}
int(0)
int(0)
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(1)
}
