--TEST--
curl in a coroutine with descriptors >= FD_SETSIZE: a socket curl_multi_fdset() cannot show still wakes the wait promptly (#4)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip requires proc_open');
if (trim((string) shell_exec('ulimit -n 4096 2>/dev/null && echo ok')) !== 'ok') die('skip cannot raise the descriptor limit to 4096');
if (trim((string) shell_exec(escapeshellarg(PHP_BINARY) . ' -r "echo function_exists(\'curl_exec\') ? 1 : 0;"')) !== '1') die('skip requires ext/curl');
?>
--FILE--
<?php
// /fast replies at once, anything else after 300ms; one process per connection, so requests can overlap.
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
        if ($due === null && in_array($c, $r, true)) { $cs[$k][1] = microtime(true) + (str_contains(fread($c, 8192), "GET /fast") ? 0 : 0.3); }
        elseif ($due !== null && microtime(true) >= $due) { fwrite($c, "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"); fclose($c); unset($cs[$k]); }
    }
}';
$srv = proc_open([PHP_BINARY, '-n', '-r', $server], [1 => ['pipe', 'w']], $pipes);
$port = trim(fgets($pipes[1]));

$child = tempnam(sys_get_temp_dir(), 'phasync_ch_') . '.php';
file_put_contents($child, '<?php $port = ' . (int) $port . '; require ' . var_export(__DIR__ . '/loop.inc', true) . ';' . <<<'CHILD'

function req(string $path = '/'): CurlHandle {
    global $port;
    $ch = curl_init("http://127.0.0.1:$port$path");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    return $ch;
}
$keep = [];
for ($i = 0; $i < 1100; $i++) $keep[] = fopen('/dev/null', 'r');

// Every socket of curl's is above FD_SETSIZE.
$l = new Loop; $t = microtime(true);
$l->runAll(function () use (&$r) { $r = curl_exec(req()); });
$d = microtime(true) - $t;
echo "curl_exec: $r, ", $d < 0.6 && $l->parks <= 40 ? "prompt" : sprintf("%.3fs, %d parks", $d, $l->parks), "\n";

$t = microtime(true);
(new Loop)->runAll(function () { curl_exec(req()); }, function () { curl_exec(req()); });
$d = microtime(true) - $t;
echo $d < 0.55 ? "concurrent\n" : sprintf("sequential (%.3fs)\n", $d);

// The slow transfer's socket below FD_SETSIZE, the fast one's above: fdset()
// shows only the slow one, yet the fast one finishes long before it.
(new Loop)->runAll(function () use (&$keep, &$fast) {
    $mh = curl_multi_init();
    fclose(array_shift($keep));                  // one free descriptor below FD_SETSIZE
    curl_multi_add_handle($mh, $slow = req());
    curl_multi_exec($mh, $running);              // the slow one connects on it
    curl_multi_add_handle($mh, $f = req('/fast'));
    $t = microtime(true);
    do {
        curl_multi_exec($mh, $running);
        while ($info = curl_multi_info_read($mh)) if ($info['handle'] === $f) $fast = microtime(true) - $t;
        if ($running) curl_multi_select($mh, 5.0);
    } while ($running);
});
echo $fast < 0.15 ? "fast transfer prompt\n" : sprintf("fast transfer late (%.3fs)\n", $fast);
CHILD);
$so = realpath(ini_get('extension_dir') . '/phasync.so');
passthru('ulimit -n 4096 && exec ' . escapeshellarg(PHP_BINARY) . ' -d extension=' . escapeshellarg($so) . ' ' . escapeshellarg($child) . ' 2>&1');
unlink($child);
proc_terminate($srv);
fclose($pipes[1]);
proc_close($srv);
?>
--EXPECT--
curl_exec: hello, prompt
concurrent
fast transfer prompt
