--TEST--
sapi(): exit() in $code still runs the request's shutdown functions, in the request, before its output is flushed; exit() in one of them skips the rest, as PHP does (#19)
--EXTENSIONS--
phasync
--FILE--
<?php
foreach ([
    'echo "before "; exit(5);',
    'register_shutdown_function(function () { echo "exits "; exit(7); }); echo "before ";',
] as $body) {
    $code = <<<PHP
        require \$argv[1];
        \$s = new Sink;
        register_shutdown_function(function () use (\$s) { echo "worker: ", json_encode(\$s->out), "\\n"; });
        phasync\\ext\\sapi(function () {
            ob_start();
            register_shutdown_function(function () { echo "request "; });
            $body
            register_shutdown_function(function () { echo "skipped "; });
        }, \$s);
        echo "never\\n";
    PHP;
    passthru(escapeshellarg(PHP_BINARY) . ' -d extension=' . escapeshellarg(realpath(ini_get('extension_dir') . '/phasync.so'))
        . ' -r ' . escapeshellarg($code) . ' ' . escapeshellarg(__DIR__ . '/sink.inc'), $status);
    echo "exit status: $status\n";
}
?>
--EXPECT--
worker: "before request "
exit status: 5
worker: "before request exits "
exit status: 7
