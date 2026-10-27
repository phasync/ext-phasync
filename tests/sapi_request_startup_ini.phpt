--TEST--
sapi(): with enable_post_data_reading=0 a form body stays in php://input; register_argc_argv derives $_SERVER['argv'] from the query string, as for a web request (#10)
--EXTENSIONS--
phasync
--INI--
enable_post_data_reading=0
register_argc_argv=1
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$s = new Sink;
$s->body = 'p=1';
$s->info = ['method' => 'POST', 'content_type' => 'application/x-www-form-urlencoded', 'content_length' => 3, 'query_string' => 'x+y'];
sapi(fn() => print(json_encode([$_POST, file_get_contents('php://input'), $_SERVER['argv'], $_SERVER['argc']]) . "\n"), $s);
echo $s->out;
?>
--EXPECTF--
%A[[],"p=1",["x","y"],2]
