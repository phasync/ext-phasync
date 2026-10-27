--TEST--
sapi(): a request's $_GET, $_POST, $_COOKIE, $_FILES, $_SERVER and $_REQUEST are registered by PHP's own request startup into the current Scope's superglobals; concurrent requests in their own Scopes each see theirs, and filter_input() the request's own input; the host's are untouched (#10)
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
$_SERVER = ['w' => 'server'];
$before = json_encode([$_GET, $_POST, $_COOKIE, $_FILES, $_REQUEST, $_SERVER]);

$a = new Sink;
$a->body = 'name=alice';
// $_REQUEST follows request_order (run-tests sets GP)
$a->info = ['method' => 'POST', 'content_type' => 'application/x-www-form-urlencoded', 'content_length' => 10,
    'query_string' => 'q=a', 'request_uri' => '/a?q=a', 'cookie' => 'c=from%20a', 'server' => ['HTTP_HOST' => 'a.test', 'my.var' => 'dot']];
$b = new Sink;
$b->info = ['method' => 'GET', 'query_string' => 'q=b&list[]=1&list[]=2', 'server' => ['HTTP_HOST' => 'b.test']];

$log = [];
$request = function (string $who) use (&$log) { return function () use ($who, &$log) {
    Fiber::suspend();                     // the other request starts meanwhile
    $log[] = "$who: " . json_encode([$_GET, $_POST, $_COOKIE, $_FILES, $_SERVER['HTTP_HOST'], $_SERVER['my_var'] ?? null,
        is_int($_SERVER['REQUEST_TIME']), $_REQUEST]);
    $log[] = "$who filter: " . json_encode([filter_input(INPUT_GET, 'q'), filter_input(INPUT_POST, 'name'),
        filter_input(INPUT_COOKIE, 'c'), filter_input(INPUT_SERVER, 'HTTP_HOST')]);
    $f = new Fiber(function () use ($who, &$log) { $log[] = "$who fiber: " . $_GET['q']; });
    $f->start();
}; };
$fa = new Fiber(fn() => (new Scope)->run(fn() => sapi($request('a'), $a)));
$fb = new Fiber(fn() => (new Scope)->run(fn() => sapi($request('b'), $b)));
$fa->start();
$fb->start();
$fa->resume();
$fb->resume();
echo implode("\n", $log), "\n";
var_dump(json_encode([$_GET, $_POST, $_COOKIE, $_FILES, $_REQUEST, $_SERVER]) === $before);
var_dump(filter_input(INPUT_GET, 'q'), filter_has_var(INPUT_SERVER, 'HTTP_HOST'));
?>
--EXPECT--
a: [{"q":"a"},{"name":"alice"},{"c":"from a"},[],"a.test","dot",true,{"q":"a","name":"alice"}]
a filter: ["a","alice","from a","a.test"]
a fiber: a
b: [{"q":"b","list":["1","2"]},[],[],[],"b.test",null,true,{"q":"b","list":["1","2"]}]
b filter: ["b",null,null,"b.test"]
b fiber: b
bool(true)
NULL
bool(false)
