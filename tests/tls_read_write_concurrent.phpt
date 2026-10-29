--TEST--
A coroutine writing a blocking tls:// stream while another is suspended reading it writes cooperatively and whole (the reader's non-blocking switch is not the stream's mode)
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
$size = 4 << 20;

// A slow TLS peer: reads one line, then answers with its length.
$server = '
$ctx = stream_context_create(["ssl" => ["local_cert" => ' . var_export($pemFile, true) . ', "verify_peer" => false]]);
$s = stream_socket_server("tls://127.0.0.1:0", $e, $es, STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $ctx);
$name = stream_socket_get_name($s, false); echo "PORT:" . substr($name, strrpos($name, ":") + 1) . "\n"; flush();
$c = stream_socket_accept($s, 10);
$got = ""; usleep(300000);
while (!str_contains($got, "\n") && !feof($c)) { $got .= fread($c, 65536); usleep(1000); }
fwrite($c, "got " . (strlen($got) - 1) . "\n");
fread($c, 1);
';
$p = proc_open([PHP_BINARY, '-n', '-r', $server], [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
while (($line = fgets($pipes[1])) !== false) {
    if (preg_match('/PORT:(\d+)/', $line, $m)) { $port = $m[1]; break; }
}
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$c = stream_socket_client("tls://127.0.0.1:$port", $e, $es, 5, STREAM_CLIENT_CONNECT, $ctx);
$log = [];
(new Loop)->runAll(
    function () use ($c, &$log) { $log[] = 'reader: ' . trim(fgets($c)); },
    function () use ($c, $size, &$log) { $log[] = 'writer: ' . fwrite($c, str_repeat('x', $size) . "\n"); },
);
sort($log);
echo implode("\n", $log), "\n";
fclose($c);
foreach ($pipes as $pp) fclose($pp);
proc_close($p);
unlink($pemFile);
?>
--EXPECT--
reader: got 4194304
writer: 4194305
