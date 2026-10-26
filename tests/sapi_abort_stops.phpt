--TEST--
sapi(): a client gone stops the request at that output (zend_bailout()), as php-fpm does; a shutdown function still runs
--EXTENSIONS--
phasync
--FILE--
<?php
$code = <<<'PHP'
    require $argv[1];
    $s = new Sink; $s->gone = true;
    register_shutdown_function(function () { fwrite(STDERR, "shutdown ran\n"); });
    phasync\ext\sapi(function () {
        echo "one";
        echo "two";
        fwrite(STDERR, "unreachable\n");
    }, $s);
    fwrite(STDERR, "after sapi\n");
    echo $s->out, "\n";
PHP;
passthru(escapeshellarg(PHP_BINARY) . ' -d extension=' . escapeshellarg(realpath(ini_get('extension_dir') . '/phasync.so'))
    . ' -r ' . escapeshellarg($code) . ' ' . escapeshellarg(__DIR__ . '/sink.inc') . ' 2>&1', $status);
echo "exit status: $status\n";
?>
--EXPECT--
shutdown ran
exit status: 0
