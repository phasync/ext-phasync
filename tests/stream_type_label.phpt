--TEST--
A socket the ext wraps keeps its stream type (stream_get_meta_data(), and mysqlnd's unix-socket check for caching_sha2 full authentication) (issue #63)
--EXTENSIONS--
phasync
--FILE--
<?php
$path = sys_get_temp_dir() . '/phasync-lbl-' . getmypid() . '.sock';
$srv = stream_socket_server("unix://$path");
$c = stream_socket_client("unix://$path");
$t = stream_socket_server('tcp://127.0.0.1:0');
$tc = stream_socket_client('tcp://' . stream_socket_get_name($t, false));
fwrite($c, "x");
fwrite($tc, "y");
echo stream_get_meta_data($c)['stream_type'], "\n", stream_get_meta_data($tc)['stream_type'], "\n";
unlink($path);
?>
--EXPECT--
unix_socket
tcp_socket/ssl
