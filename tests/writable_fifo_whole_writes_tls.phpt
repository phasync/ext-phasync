--TEST--
Writers on one blocking tls:// socket keep their large writes whole, in turn order (the write turn covers the TLS write path)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
if (!extension_loaded('openssl')) die('skip requires openssl');
if (!function_exists('proc_open')) die('skip requires proc_open');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
$pk = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
$crt = openssl_csr_sign(openssl_csr_new(['commonName' => 'localhost'], $pk), null, $pk, 1);
openssl_x509_export($crt, $certPem);
openssl_pkey_export($pk, $keyPem);
$pemFile = tempnam(sys_get_temp_dir(), 'phpem');
file_put_contents($pemFile, $certPem . $keyPem);
$n = 4; $size = 2 << 20;

// A slow TLS reader in a subprocess (plain PHP): reads $n lines, reports each line's letter and length.
$server = '
$ctx = stream_context_create(["ssl" => ["local_cert" => ' . var_export($pemFile, true) . ', "verify_peer" => false]]);
$s = stream_socket_server("tls://127.0.0.1:0", $e, $es, STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $ctx);
$name = stream_socket_get_name($s, false); echo "PORT:" . substr($name, strrpos($name, ":") + 1) . "\n"; flush();
$c = stream_socket_accept($s, 10);
$got = ""; usleep(300000);   // the socket buffers fill first: writers block
while (substr_count($got, "\n") < ' . $n . ' && !feof($c)) { $got .= fread($c, 65536); usleep(1000); }
foreach (explode("\n", rtrim($got, "\n")) as $l) { echo $l[0], strlen($l), " ", strspn($l, $l[0]) === strlen($l) ? "whole" : "MIXED", "\n"; }
';
$p = proc_open([PHP_BINARY, '-n', '-r', $server], [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
$port = null;
while (($line = fgets($pipes[1])) !== false) {
    if (preg_match('/PORT:(\d+)/', $line, $m)) { $port = $m[1]; break; }
}
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$c = stream_socket_client("tls://127.0.0.1:$port", $e, $es, 5, STREAM_CLIENT_CONNECT, $ctx);
$loop = new Loop;
$writers = [];
for ($i = 0; $i < $n; $i++) {
    $writers[] = function () use ($c, $i, $size) { fwrite($c, str_repeat(chr(65 + $i), $size) . "\n"); };
}
$loop->runAll(...$writers);
echo stream_get_contents($pipes[1]);
fclose($c);
foreach ($pipes as $pp) fclose($pp);
proc_close($p);
unlink($pemFile);
?>
--EXPECT--
A2097152 whole
B2097152 whole
C2097152 whole
D2097152 whole
