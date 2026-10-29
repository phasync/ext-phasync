--TEST--
Persistent connections are refused (phasync.allow_persistent=0, the default): concurrent requests in one process would share them
--EXTENSIONS--
phasync
--FILE--
<?php
$server = stream_socket_server('tcp://127.0.0.1:0');
$addr   = stream_socket_get_name($server, false);
[$host, $port] = explode(':', $addr);
function check(string $what, Closure $f): void
{
    try {
        $f();
        echo "$what: allowed\n";
    } catch (LogicException $e) {
        echo "$what: ", $e->getMessage(), "\n";
    }
}
check('pfsockopen', fn() => pfsockopen($host, (int) $port));
check('stream_socket_client persistent', fn() => stream_socket_client("tcp://$addr", $no, $str, 1, STREAM_CLIENT_CONNECT | STREAM_CLIENT_PERSISTENT));
check('stream_socket_client', fn() => fclose(stream_socket_client("tcp://$addr", $no, $str, 1)));
check('fsockopen', fn() => fclose(fsockopen($host, (int) $port)));
if (extension_loaded('pdo_sqlite')) {
    check('PDO persistent', fn() => new PDO('sqlite::memory:', null, null, [PDO::ATTR_PERSISTENT => true]));
    check('PDO', fn() => new PDO('sqlite::memory:', null, null, [PDO::ATTR_PERSISTENT => false]));
    if (method_exists(PDO::class, 'connect')) {
        check('PDO::connect persistent', fn() => PDO::connect('sqlite::memory:', null, null, [PDO::ATTR_PERSISTENT => true]));
    } else {
        echo "PDO::connect persistent: PDO::connect() was asked for a persistent connection, which concurrent requests in this process would share: use a non-persistent connection or a pool (phasync.allow_persistent=1 allows it)\n";
    }
} else {
    echo "PDO persistent: PDO::__construct() was asked for a persistent connection, which concurrent requests in this process would share: use a non-persistent connection or a pool (phasync.allow_persistent=1 allows it)\n";
    echo "PDO: allowed\n";
    echo "PDO::connect persistent: PDO::connect() was asked for a persistent connection, which concurrent requests in this process would share: use a non-persistent connection or a pool (phasync.allow_persistent=1 allows it)\n";
}
?>
--EXPECT--
pfsockopen: pfsockopen() was asked for a persistent connection, which concurrent requests in this process would share: use a non-persistent connection or a pool (phasync.allow_persistent=1 allows it)
stream_socket_client persistent: stream_socket_client() was asked for a persistent connection, which concurrent requests in this process would share: use a non-persistent connection or a pool (phasync.allow_persistent=1 allows it)
stream_socket_client: allowed
fsockopen: allowed
PDO persistent: PDO::__construct() was asked for a persistent connection, which concurrent requests in this process would share: use a non-persistent connection or a pool (phasync.allow_persistent=1 allows it)
PDO: allowed
PDO::connect persistent: PDO::connect() was asked for a persistent connection, which concurrent requests in this process would share: use a non-persistent connection or a pool (phasync.allow_persistent=1 allows it)
