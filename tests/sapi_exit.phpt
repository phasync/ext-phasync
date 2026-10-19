--TEST--
sapi(): exit() inside a request is not intercepted -- the output is flushed to $sapi, then PHP ends the script with exit()'s status as it always does
--EXTENSIONS--
phasync
--FILE--
<?php
$code = <<<'PHP'
    require $argv[1];
    $s = new Sink;
    register_shutdown_function(function () use ($s) { echo "shutdown: ", json_encode($s->out), "\n"; });
    phasync\ext\sapi(function () { echo "before "; exit(5); echo "never"; }, $s);
    echo "never\n";
PHP;
passthru(escapeshellarg(PHP_BINARY) . ' -d extension=' . escapeshellarg(realpath(ini_get('extension_dir') . '/phasync.so'))
    . ' -r ' . escapeshellarg($code) . ' ' . escapeshellarg(__DIR__ . '/sink.inc'), $status);
echo "exit status: $status\n";
?>
--EXPECT--
shutdown: "before "
exit status: 5
