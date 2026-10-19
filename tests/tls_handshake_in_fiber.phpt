--TEST--
TLS handshake (tls:// connect, stream_socket_enable_crypto()) parks the coroutine inside a scope; enabling/disabling crypto re-wraps the stream's reads
--EXTENSIONS--
phasync
openssl
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
if (!function_exists('proc_open')) die('skip requires proc_open');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';

$pk  = openssl_pkey_new(['private_key_bits' => 2048, 'private_key_type' => OPENSSL_KEYTYPE_RSA]);
$csr = openssl_csr_new(['commonName' => 'localhost'], $pk);
openssl_x509_export(openssl_csr_sign($csr, null, $pk, 1), $crt);
openssl_pkey_export($pk, $key);
$pem = tempnam(sys_get_temp_dir(), 'phpem');
file_put_contents($pem, $crt . $key);

// A plain PHP server. Each connection first says what to do in plain text:
// "tls": wait 300ms, then do its side of the handshake, send "secret" over TLS,
// then turn TLS off again and send "plain"; "close": hang up after the ClientHello.
$server = '
$c = stream_context_create(["ssl" => ["local_cert" => ' . var_export($pem, true) . ', "verify_peer" => false]]);
$s = stream_socket_server("tcp://127.0.0.1:0", $e, $es, STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $c);
$t = stream_socket_server("tcp://127.0.0.1:0", $e, $es, STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $c);
foreach ([$s, $t] as $l) { $n = stream_socket_get_name($l, false); echo substr($n, strrpos($n, ":") + 1), "\n"; }
flush();
while (true) {
    $r = [$s, $t]; $w = $ex = null;
    if (!stream_select($r, $w, $ex, 10)) break;
    $l = reset($r);
    $x = stream_socket_accept($l);
    stream_set_read_buffer($x, 0);                   // the mode line only: the ClientHello right behind it is OpenSSL\'s
    $mode = "";
    if ($l !== $t) while (($ch = fread($x, 1)) !== "" && $ch !== false && $ch !== "\\n") $mode .= $ch;
    if ($l === $t) $mode = "tls";                    // $t: a tls:// client, no mode line
    if ($mode === "close") {                         // take the whole ClientHello first: a close with unread
        $h = "";                                     // data resets, and the client sees either, by timing
        while (strlen($h) < 5 && ($d = fread($x, 5 - strlen($h))) !== "" && $d !== false) $h .= $d;
        for ($n = strlen($h) === 5 ? unpack("n", $h, 3)[1] : 0; $n > 0 && ($d = fread($x, $n)) !== "" && $d !== false; $n -= strlen($d));
    }
    if ($mode === "tls") {
        usleep(300000);
        stream_socket_enable_crypto($x, true, STREAM_CRYPTO_METHOD_TLS_SERVER);
        fwrite($x, "secret");
        fgets($x);                                   // the client is done with TLS
        @stream_socket_enable_crypto($x, false);
        fwrite($x, "plain");
        stream_socket_shutdown($x, STREAM_SHUT_WR);  // read the client\'s close-notify before closing: unread
        while (!feof($x) && fread($x, 8192) !== false);  // data makes close() reset, which can drop "plain" in flight
    }
    fclose($x);
}';
$p = proc_open([PHP_BINARY, '-n', '-r', $server], [1 => ['pipe', 'w']], $pipes);
$port = trim(fgets($pipes[1]));
$tport = trim(fgets($pipes[1]));
$ctx = ['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]];

function coop(string $name, Closure $op): mixed {
    $ticks = 0; $done = false; $res = null;
    (new Loop)->runAll(
        function () use ($op, &$done, &$res) { $res = $op(); $done = true; },
        function () use (&$ticks, &$done) { while (!$done) { usleep(20000); $ticks++; } },
    );
    printf("%-34s %s\n", $name, $ticks >= 3 ? 'cooperative' : "BLOCKED (ticks=$ticks)");
    return $res;
}

// stream_socket_enable_crypto() on a blocking tcp:// stream; the reads after it
// are decrypted (TLS), and plain again once crypto is off.
$r = coop('stream_socket_enable_crypto', function () use ($port, $ctx) {
    $c = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 5, STREAM_CLIENT_CONNECT, stream_context_create($ctx));
    fwrite($c, "tls\n");
    $on = stream_socket_enable_crypto($c, true, STREAM_CRYPTO_METHOD_TLS_CLIENT);
    $secret = fread($c, 100);
    fwrite($c, "done\n");
    @stream_socket_enable_crypto($c, false);
    $plain = '';
    while (!feof($c) && !str_ends_with($plain, 'plain')) $plain .= fread($c, 100);
    return [$on, $secret, str_ends_with($plain, 'plain')];   // after the peer's TLS close-notify
});
var_dump($r);

// tls:// connect: the handshake inside the connect parks too.
$r = coop('tls:// connect', function () use ($tport, $ctx) {
    $c = stream_socket_client("tls://127.0.0.1:$tport", $e, $es, 5, STREAM_CLIENT_CONNECT, stream_context_create($ctx));
    return fread($c, 100);
});
var_dump($r);

// A peer that hangs up during the handshake: the same result and warning as native.
$fail = function () use ($port, $ctx) {
    $c = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 5, STREAM_CLIENT_CONNECT, stream_context_create($ctx));
    fwrite($c, "close\n");
    error_clear_last();
    $r = @stream_socket_enable_crypto($c, true, STREAM_CRYPTO_METHOD_TLS_CLIENT);
    return [$r, preg_replace('/^.*?\(\): /', '', error_get_last()['message'] ?? '')];
};
$native = $fail();
$ext = null;
(new Loop)->runAll(function () use ($fail, &$ext) { $ext = $fail(); });
echo 'handshake failure: ', $native === $ext ? 'same' : 'DIFF ' . json_encode([$native, $ext]), "\n";

// A non-blocking stream stays native: one attempt, 0 while it would block.
$l = new Loop;
$l->runAll(function () use ($port, $ctx, $l, &$r) {
    $c = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 5, STREAM_CLIENT_CONNECT, stream_context_create($ctx));
    fwrite($c, "tls\n");
    stream_set_blocking($c, false);
    $before = $l->parks;
    $r = [stream_socket_enable_crypto($c, true, STREAM_CRYPTO_METHOD_TLS_CLIENT), $l->parks - $before];
});
var_dump($r);

proc_terminate($p);
fclose($pipes[1]);
proc_close($p);
@unlink($pem);
?>
--EXPECT--
stream_socket_enable_crypto        cooperative
array(3) {
  [0]=>
  bool(true)
  [1]=>
  string(6) "secret"
  [2]=>
  bool(true)
}
tls:// connect                     cooperative
string(6) "secret"
handshake failure: same
array(2) {
  [0]=>
  int(0)
  [1]=>
  int(0)
}
