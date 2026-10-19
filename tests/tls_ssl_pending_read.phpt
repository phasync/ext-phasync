--TEST--
A hooked tls:// read while OpenSSL holds decrypted data the stream buffer doesn't (SSL_pending()): no recursion through the cast for select() (the PrestaShop stack overflow)
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
// self-signed cert, generated in-process
$pk = openssl_pkey_new(['private_key_bits'=>2048, 'private_key_type'=>OPENSSL_KEYTYPE_RSA]);
$csr = openssl_csr_new(['commonName'=>'localhost'], $pk);
$crt = openssl_csr_sign($csr, null, $pk, 1);
openssl_x509_export($crt, $certPem);
openssl_pkey_export($pk, $keyPem);
$pemFile = tempnam(sys_get_temp_dir(), 'phpem');
file_put_contents($pemFile, $certPem.$keyPem);

// TLS echo-ish server in a subprocess (plain PHP, no extension needed)
$server = '
$ctx = stream_context_create(["ssl"=>["local_cert"=>' . var_export($pemFile, true) . ',"verify_peer"=>false]]);
$s = stream_socket_server("tls://127.0.0.1:0",$e,$es,STREAM_SERVER_BIND|STREAM_SERVER_LISTEN,$ctx);
$n = stream_socket_get_name($s,false); echo "PORT:".substr($n,strrpos($n,":")+1)."\n"; flush();
$c = @stream_socket_accept($s,5);
if($c){ usleep(100000); fwrite($c,str_repeat("abcdefghij",1600)); usleep(300000); fclose($c); }
';
$p = proc_open([PHP_BINARY, '-n', '-r', $server], [1=>['pipe','w'], 2=>['pipe','w']], $pipes);
$port = null;
while (($line = fgets($pipes[1])) !== false) {
    if (preg_match('/PORT:(\d+)/', $line, $m)) { $port = $m[1]; break; }
}

$ctx = stream_context_create(['ssl'=>['verify_peer'=>false,'verify_peer_name'=>false]]);

$loop = new Loop;
$loop->runAll(function () use ($port, $ctx) {
    $c = @stream_socket_client("tls://127.0.0.1:$port",$e,$es,5,STREAM_CLIENT_CONNECT,$ctx);
    $got = '';
    while (strlen($got) < 16000 && !feof($c)) { $got .= fread($c, 1000); }
    echo "tls fiber read: ", strlen($got), " bytes, intact: ", $got === str_repeat('abcdefghij', 1600) ? 'yes' : 'no', "\n";
});
var_dump($loop->parks > 0);   // proves it went async, not blocking
foreach ($pipes as $pp) @fclose($pp);
proc_close($p);
@unlink($pemFile);
?>
--EXPECT--
tls fiber read: 16000 bytes, intact: yes
bool(true)
