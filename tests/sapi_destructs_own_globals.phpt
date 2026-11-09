--TEST--
sapi(): a top-level global include_global() declared is destroyed by name after the request's
shutdown functions and before its output flush/headers sent, as php_request_shutdown()'s
zend_call_destructors() would for a fresh php-fpm process -- PrestaShop's Cookie::__destruct()
(held in a global, not the static case: see ext-phasync#25) otherwise never reaches the
response (#24 follow-up)
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--INI--
output_buffering=4096
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;
use function phasync\ext\include_global;

// A global that exists before any request: never swept, since no request owns it.
$sapiDestructsOwnGlobalsSeq = 'pre-existing';

$a = new Sink;
$a->info = ['method' => 'GET', 'query_string' => ''];
sapi(function () {
    $GLOBALS['sapiDestructsOwnGlobalsSeq'] = 'req1';
    include_global(__DIR__ . '/sapi_destructs_own_globals.inc');
    // Still alive here: the request has not ended yet.
    var_dump(isset($GLOBALS['owned']));
}, $a);
echo $a->out;
echo implode("\n", $a->headers[2]), "\n";
var_dump(isset($GLOBALS['owned']));                                  // gone once the request ended
var_dump($sapiDestructsOwnGlobalsSeq);                               // pre-existing: not unset, only its value changed

// A second, independent request reusing the same include file: its own object, its own destructor.
$b = new Sink;
$b->info = ['method' => 'GET', 'query_string' => ''];
sapi(function () {
    $GLOBALS['sapiDestructsOwnGlobalsSeq'] = 'req2';
    include_global(__DIR__ . '/sapi_destructs_own_globals.inc');
}, $b);
echo $b->out;
echo implode("\n", $b->headers[2]), "\n";
?>
--EXPECTF--
body req1
bool(true)
X-Powered-By: PHP/%s
X-Shutdown: req1
X-Destructed: req1
Content-type: text/html; charset=UTF-8
bool(false)
string(4) "req1"
body req2
X-Powered-By: PHP/%s
X-Shutdown: req2
X-Destructed: req2
Content-type: text/html; charset=UTF-8
