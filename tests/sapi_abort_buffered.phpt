--TEST--
sapi(): a client gone discovered while flushing a buffered write at the end of the request also stops it cleanly (no second, uncaught bailout)
--EXTENSIONS--
phasync
--FILE--
<?php
$code = <<<'PHP'
    ini_set('output_buffering', '4096');
    require $argv[1];
    $s = new Sink; $s->gone = true;
    register_shutdown_function(function () { fwrite(STDERR, "shutdown ran\n"); });
    phasync\ext\sapi(function () {
        echo "one";
    }, $s);
    fwrite(STDERR, "after sapi\n");
PHP;
passthru(escapeshellarg(PHP_BINARY) . ' -d extension=' . escapeshellarg(realpath(ini_get('extension_dir') . '/phasync.so'))
    . ' -d output_buffering=4096 -r ' . escapeshellarg($code) . ' ' . escapeshellarg(__DIR__ . '/sink.inc') . ' 2>&1', $status);
echo "exit status: $status\n";
?>
--EXPECT--
shutdown ran
exit status: 0
