--TEST--
phasync.allow_persistent=1 restores PHP's persistent connections
--EXTENSIONS--
phasync
--INI--
phasync.allow_persistent=1
--FILE--
<?php
$server = stream_socket_server('tcp://127.0.0.1:0');
[$host, $port] = explode(':', stream_socket_get_name($server, false));
var_dump(is_resource(pfsockopen($host, (int) $port)));
if (extension_loaded('pdo_sqlite')) {
    var_dump((new PDO('sqlite::memory:', null, null, [PDO::ATTR_PERSISTENT => true]))->getAttribute(PDO::ATTR_PERSISTENT));
} else {
    var_dump(true);
}
?>
--EXPECT--
bool(true)
bool(true)
