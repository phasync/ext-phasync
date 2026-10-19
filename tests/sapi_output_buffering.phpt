--TEST--
sapi(): a request starts with output_buffering's default buffer (or output_handler, or implicit_flush), as php_request_startup() does (issue #9)
--EXTENSIONS--
phasync
--INI--
output_buffering=4096
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$worker = ob_get_level();
$s = new Sink;
sapi(function () {
    echo ob_get_level(), ' ', ob_get_status()['chunk_size'], ' ', ob_get_status()['name'], "\n";
    ob_flush();
    flush();
    echo "end\n";
}, $s);
echo $s->out, $s->flushes > 0 ? "flushed\n" : "not flushed\n";
echo ob_get_level() === $worker ? "worker unchanged\n" : "worker changed\n";
?>
--EXPECT--
1 4096 default output handler
end
flushed
worker unchanged
