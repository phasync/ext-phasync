--TEST--
Poller: a coroutine started with a TCP stream as its first argument suspends in its waits; OpenSSL labels every tcp:// stream, and only its renegotiation callback (inside SSL_read) must never suspend
--EXTENSIONS--
phasync
openssl
--FILE--
<?php
require __DIR__ . '/loop.inc';
$server = stream_socket_server('tcp://127.0.0.1:0');
$client = stream_socket_client('tcp://' . stream_socket_get_name($server, false));
$peer   = stream_socket_accept($server);
echo stream_get_meta_data($client)['stream_type'], "\n";
$loop = new Loop;
$log  = [];
$loop->manage(function () use ($loop, $client, $peer, &$log) {
    $loop->go(function ($conn) use ($loop, &$log) {
        $loop->poller->readable($conn, 5);
        $log[] = 'readable';
    }, $client);
    $loop->go(function () use ($peer, &$log) {
        $log[] = 'other coroutine runs';
        usleep(100000);
        fwrite($peer, 'x');
    });
    $loop->run();
});
echo implode("\n", $log), "\n";
?>
--EXPECT--
tcp_socket/ssl
other coroutine runs
readable
