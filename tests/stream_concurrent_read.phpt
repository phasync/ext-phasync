--TEST--
A coroutine reading a blocking stream another coroutine is suspended reading gets a LogicException at once; the first read is undisturbed; writes are not guarded (#6)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/loop.inc';

[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
$loop = new Loop;
$loop->runAll(
    function () use ($a) { $r = fread($a, 5); echo "first: $r\n"; },
    function () use ($a, $b) {
        usleep(100000);
        foreach (['fread' => fn() => fread($a, 5), 'fgets' => fn() => fgets($a),
                  'stream_get_contents' => fn() => stream_get_contents($a, 5),
                  'stream_socket_recvfrom' => fn() => stream_socket_recvfrom($a, 5)] as $name => $read) {
            try { var_dump($read()); }
            catch (LogicException $e) { echo "$name: ", $e->getMessage(), "\n"; }
        }
        echo 'write while it reads: ', fwrite($a, 'ping'), "\n";
        echo 'peer got: ', fread($b, 4), "\n";
        fwrite($b, 'hello');
    },
);
fwrite($b, 'again');
$loop->runAll(function () use ($a) { echo 'afterwards: ', fread($a, 5), "\n"; });
?>
--EXPECT--
fread: Concurrent operations on the same resource are not supported
fgets: Concurrent operations on the same resource are not supported
stream_get_contents: Concurrent operations on the same resource are not supported
stream_socket_recvfrom: Concurrent operations on the same resource are not supported
write while it reads: 4
peer got: ping
first: hello
afterwards: again
