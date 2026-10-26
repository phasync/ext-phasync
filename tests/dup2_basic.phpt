--TEST--
dup2(): duplicates a stream's descriptor onto a given fd; it survives the original closing
--EXTENSIONS--
phasync
--FILE--
<?php
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
phasync\ext\dup2($a, 7);
fclose($a);
$out = fopen('php://fd/7', 'w');
fwrite($out, "hello");
fclose($out);
echo fread($b, 5), "\n";
fclose($b);
?>
--EXPECT--
hello
