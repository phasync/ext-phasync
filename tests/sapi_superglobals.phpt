--TEST--
sapi() does not touch any superglobal: $_GET, $_POST, $_COOKIE, $_FILES and $_REQUEST are the host's, and $_SERVER is a plain global shared by all requests
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--INI--
variables_order=EGPCS
request_order=GP
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$_GET = ['w' => 'get'];
$_POST = ['w' => 'post'];
$_COOKIE = ['w' => 'cookie'];
$_FILES = ['w' => 'files'];
$_REQUEST = ['w' => 'request'];
$_SERVER = ['w' => 'server'];
$all = fn() => json_encode([$_GET, $_POST, $_COOKIE, $_FILES, $_REQUEST, $_SERVER]);
$before = $all();

// A request with a query string, a form body, a Cookie header: sapi() builds nothing from them.
$a = new Sink;
$a->body = 'name=alice';
$a->info = ['method' => 'POST', 'content_type' => 'application/x-www-form-urlencoded',
    'content_length' => 10, 'query_string' => 'q=a', 'request_uri' => '/a?q=a'];
$b = new Sink;
$b->info = ['method' => 'GET', 'query_string' => 'q=b'];

$log = [];
$request = function (string $who) use (&$log, $all, $before) { return function () use ($who, &$log, $all, $before) {
    $log[] = "$who before: " . ($all() === $before ? 'untouched' : 'TOUCHED');
    Fiber::suspend();
    // $_SERVER is shared: what one request writes the others (and the host) see.
    $_SERVER[$who] = 1;
    Fiber::suspend();
    $log[] = "$who sees: " . implode(',', array_keys($_SERVER));
    $f = new Fiber(function () use ($who, &$log) { $log[] = "$who fiber sees: " . implode(',', array_keys($_SERVER)); });
    $f->start();
}; };
$fa = new Fiber(fn() => sapi($request('a'), $a));
$fb = new Fiber(fn() => sapi($request('b'), $b));
$fa->start();
$fb->start();
$fa->resume();
$fb->resume();
$fa->resume();
$fb->resume();
echo implode("\n", $log), "\n";
var_dump(array_keys($_SERVER));
unset($_SERVER['a'], $_SERVER['b']);
var_dump($all() === $before);

// A request replacing a superglobal replaces the host's.
sapi(function () { $_GET = ['changed' => 1]; $_SERVER = ['changed' => 1]; }, new Sink);
var_dump($_GET, $_SERVER);
?>
--EXPECT--
a before: untouched
b before: untouched
a sees: w,a,b
a fiber sees: w,a,b
b sees: w,a,b
b fiber sees: w,a,b
array(3) {
  [0]=>
  string(1) "w"
  [1]=>
  string(1) "a"
  [2]=>
  string(1) "b"
}
bool(true)
array(1) {
  ["changed"]=>
  int(1)
}
array(1) {
  ["changed"]=>
  int(1)
}
