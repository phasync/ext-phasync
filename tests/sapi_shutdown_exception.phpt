--TEST--
sapi(): a request's shutdown functions run after $code throws; an exception in one skips the rest (as PHP's fatal "Uncaught" does) and leaves sapi() as one from $code; the output is still flushed (#19)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$s = new Sink;
try {
    sapi(function () {
        register_shutdown_function(function () { echo "runs\n"; });
        throw new Exception('code');
    }, $s);
} catch (Exception $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
echo $s->out;

$s = new Sink;
try {
    sapi(function () {
        ob_start();
        register_shutdown_function(function () { echo "first\n"; throw new Exception('shutdown'); });
        register_shutdown_function(function () { echo "skipped\n"; });
        echo "buffered\n";
    }, $s);
} catch (Exception $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
echo $s->out;

$s = new Sink;
try {
    sapi(function () {
        register_shutdown_function(function () { throw new Exception('shutdown'); });
        throw new Exception('code');
    }, $s);
} catch (Exception $e) {
    echo $e->getMessage(), " <- ", $e->getPrevious()?->getMessage(), "\n";
}
?>
--EXPECT--
Exception: code
runs
Exception: shutdown
buffered
first
shutdown <- code
