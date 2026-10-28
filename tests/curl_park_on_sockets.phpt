--TEST--
curl_exec()/curl_multi_select() in a coroutine park on curl's sockets: no polling wake-ups, slow requests overlap (#4)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip requires proc_open');
if (trim((string) shell_exec(escapeshellarg(PHP_BINARY) . ' -r "echo function_exists(\'curl_exec\') ? 1 : 0;"')) !== '1') die('skip requires ext/curl');
?>
--FILE--
<?php
// Replies after 300ms; one process per connection, so requests can overlap.
$server = '
$s = stream_socket_server("tcp://127.0.0.1:0");
$n = stream_socket_get_name($s, false); echo substr($n, strrpos($n, ":") + 1), "\n"; flush();
$cs = [];
while (true) {
    $r = [$s]; foreach ($cs as $c) $r[] = $c[0]; $w = null; $e = null;
    $t = 1.0; foreach ($cs as $c) if ($c[1] !== null) $t = min($t, max(0, $c[1] - microtime(true)));
    if (stream_select($r, $w, $e, 0, (int) ($t * 1e6)) === false) break;
    if (in_array($s, $r, true) && ($c = @stream_socket_accept($s, 0))) $cs[(int) $c] = [$c, null];
    foreach ($cs as $k => [$c, $due]) {
        if ($due === null && in_array($c, $r, true)) { fread($c, 8192); $cs[$k][1] = microtime(true) + 0.3; }
        elseif ($due !== null && microtime(true) >= $due) { fwrite($c, "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"); fclose($c); unset($cs[$k]); }
    }
}';
$srv = proc_open([PHP_BINARY, '-n', '-r', $server], [1 => ['pipe', 'w']], $pipes);
$port = trim(fgets($pipes[1]));

$child = tempnam(sys_get_temp_dir(), 'phasync_cp_') . '.php';
file_put_contents($child, '<?php $port = ' . (int) $port . '; require ' . var_export(__DIR__ . '/loop.inc', true) . ';' . <<<'CHILD'

function req(): CurlHandle {
    global $port;
    $ch = curl_init("http://127.0.0.1:$port/");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    return $ch;
}

// One request waits ~300ms: a handful of parks, not a poll every few ms.
$l = new Loop;
$l->runAll(function () use (&$r) { $r = curl_exec(req()); });
echo "curl_exec: $r, ", $l->parks <= 6 ? "parked" : "polled ({$l->parks} parks)", "\n";

$l = new Loop;
$l->runAll(function () use (&$r) {
    $mh = curl_multi_init(); $ch = req(); curl_multi_add_handle($mh, $ch);
    do { curl_multi_exec($mh, $running); if ($running) curl_multi_select($mh, 5.0); } while ($running);
    $r = curl_multi_getcontent($ch);
});
echo "curl_multi_select: $r, ", $l->parks <= 6 ? "parked" : "polled ({$l->parks} parks)", "\n";

// Two slow requests in two coroutines overlap.
$t = microtime(true);
(new Loop)->runAll(function () { curl_exec(req()); }, function () { curl_exec(req()); });
$d = microtime(true) - $t;
echo $d < 0.55 ? "concurrent\n" : sprintf("sequential (%.3fs)\n", $d);
CHILD);
$so = realpath(ini_get('extension_dir') . '/phasync.so');
passthru(escapeshellarg(PHP_BINARY) . ' -d extension=' . escapeshellarg($so) . ' ' . escapeshellarg($child) . ' 2>&1');
unlink($child);
proc_terminate($srv);
fclose($pipes[1]);
proc_close($srv);
?>
--EXPECT--
curl_exec: hello, parked
curl_multi_select: hello, parked
concurrent
