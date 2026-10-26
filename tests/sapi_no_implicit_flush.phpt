--TEST--
sapi(): no implicit flush (the CLI forces implicit_flush on): every echo reaches $sapi->write() at once, $sapi->flush() runs only on flush()
--EXTENSIONS--
phasync
--INI--
output_buffering=0
implicit_flush=1
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$s = new Sink;
sapi(function () use ($s) {
    echo "a";
    echo "b";
    echo strlen($s->out), ' ', $s->flushes, "\n";
    flush();
    echo $s->flushes, "\n";
}, $s);
echo $s->out;
?>
--EXPECT--
ab2 0
1
