--TEST--
Poller: a coroutine running a closure from Scope::bind() with a TCP stream as its first argument suspends in its waits (swerve#101: the closure's C frame of the same name is no OpenSSL callback)
--EXTENSIONS--
phasync
openssl
--FILE--
<?php
require __DIR__ . '/loop.inc';
$server = stream_socket_server('tcp://127.0.0.1:0');
$client = stream_socket_client('tcp://' . stream_socket_get_name($server, false));
$peer   = stream_socket_accept($server);
$scope  = new Scope();
$loop   = new Loop;
$log    = [];
$loop->go($scope->bind(function ($conn) use ($loop, $scope, &$log) {
    $loop->poller->readable($conn, 5);
    $log[] = 'readable, in the bound scope: ' . var_export(Scope::getCurrent() === $scope, true);
    $log[] = 'read: ' . fread($conn, 1);   // a hooked read suspends too
}), $client);
$loop->go(function () use ($peer, &$log) {
    $log[] = 'other coroutine runs';
    usleep(100000);
    fwrite($peer, 'x');
});
$t = microtime(true);
$loop->run();
echo implode("\n", $log), "\n";
echo microtime(true) - $t < 1 ? "done in time\n" : "blocked\n";
?>
--EXPECT--
other coroutine runs
readable, in the bound scope: true
read: x
done in time
