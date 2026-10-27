--TEST--
stream_socket_accept() with a timeout parks the coroutine inside a scope; timeouts match native
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
require __DIR__ . '/timing.inc';
$srv  = stream_socket_server('tcp://127.0.0.1:0');
$addr = stream_socket_get_name($srv, false);

// 1. A waits in accept; B connects meanwhile.
$log = [];
$loop = new Loop;
$loop->runAll(
    function () use ($srv, &$log) {
        $c = stream_socket_accept($srv, 5, $peer);   // nothing pending yet -> parks
        $log[] = 'A accepted: ' . (is_resource($c) ? 'yes' : 'no') . ', peer is ' . (str_starts_with($peer, '127.0.0.1:') ? 'set' : var_export($peer, true));
    },
    function () use ($addr, &$log) {
        usleep(50000);
        $log[] = 'B connects';
        $GLOBALS['cli'] = stream_socket_client("tcp://$addr");
    },
);
echo implode("\n", $log), "\n";

// 2. Timeout: native warning and false; timeout 0 means don't wait (native).
$loop = new Loop;
$loop->runAll(function () use ($srv) {
    $t = microtime(true);
    var_dump(stream_socket_accept($srv, 0.1));
    var_dump(in_range(microtime(true) - $t, 0.1));  // accept's own timeout
    var_dump(stream_socket_accept($srv, 0));
});
var_dump($loop->parks);                             // int(1): only the 0.1 s one waited
?>
--EXPECTF--
B connects
A accepted: yes, peer is set

Warning: stream_socket_accept(): Accept failed: %s timed out in %s on line %d
bool(false)
bool(true)

Warning: stream_socket_accept(): Accept failed: %s timed out in %s on line %d
bool(false)
int(1)
