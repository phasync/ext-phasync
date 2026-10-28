--TEST--
curl_multi_select() parked on a busy transfer's socket wakes when another coroutine resumes a paused transfer (#4)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
if (!function_exists('proc_open')) die('skip requires proc_open');
if (trim((string) shell_exec(escapeshellarg(PHP_BINARY) . ' -r "echo function_exists(\'curl_pause\') ? 1 : 0;"')) !== '1') die('skip requires ext/curl');
?>
--FILE--
<?php
// A server in its own process sending a 4 MiB body.
$server = '
$s = stream_socket_server("tcp://127.0.0.1:0");
$n = stream_socket_get_name($s, false); echo substr($n, strrpos($n, ":") + 1), "\n"; flush();
while ($c = @stream_socket_accept($s, 20)) {
    fread($c, 8192);
    $len = 4 << 20;
    fwrite($c, "HTTP/1.0 200 OK\r\nContent-Length: $len\r\nConnection: close\r\n\r\n");
    for ($i = 0; $i < $len; $i += 65536) fwrite($c, str_repeat("x", 65536));
    fclose($c);
}';
$srv = proc_open([PHP_BINARY, '-n', '-r', $server], [1 => ['pipe', 'w']], $pipes);
$port = trim(fgets($pipes[1]));
// Accepts nothing: a request to it waits for the whole test.
$silent = stream_socket_server('tcp://127.0.0.1:0');
$sport = substr($n = stream_socket_get_name($silent, false), strrpos($n, ':') + 1);

$child = tempnam(sys_get_temp_dir(), 'phasync_cq_') . '.php';
file_put_contents($child, '<?php $port = ' . (int) $port . '; $sport = ' . (int) $sport . '; require ' . var_export(__DIR__ . '/loop.inc', true) . ';' . <<<'CHILD'

// On one multi: A buffers in its WRITEFUNCTION and pauses above 256 KiB (its
// socket leaves curl's wait set), B waits on a server that never answers. The
// other coroutine drains A's buffer and resumes it with curl_pause(CONT).
$a = curl_init("http://127.0.0.1:$port/");
$b = curl_init("http://127.0.0.1:$sport/");
curl_setopt_array($b, [CURLOPT_RETURNTRANSFER => true, CURLOPT_TIMEOUT => 20]);
$buf = ''; $total = 0; $paused = false; $pauses = 0; $done = null;
curl_setopt($a, CURLOPT_WRITEFUNCTION, function ($ch, $data) use (&$buf, &$paused, &$pauses) {
    if (strlen($buf) >= 262144) { $paused = true; $pauses++; return CURL_WRITEFUNC_PAUSE; }
    $buf .= $data;
    return strlen($data);
});
$t = microtime(true);
(new Loop)->runAll(
    function () use ($a, $b, &$done, $t) {
        $mh = curl_multi_init(); curl_multi_add_handle($mh, $b); curl_multi_add_handle($mh, $a);
        while ($done === null) {
            curl_multi_exec($mh, $running);
            while ($info = curl_multi_info_read($mh)) if ($info['handle'] === $a) $done = [$info['result'], microtime(true) - $t];
            // Older libcurl reports a paused socket ready (no wait): let the other coroutine run.
            if ($done === null && curl_multi_select($mh, 1.0) > 0) usleep(1000);
        }
        curl_multi_remove_handle($mh, $b);
    },
    function () use ($a, &$buf, &$total, &$paused, &$done) {
        while ($done === null) {
            usleep(5000);
            if ($paused) { $total += strlen($buf); $buf = ''; $paused = false; curl_pause($a, CURLPAUSE_CONT); }
        }
    },
);
$total += strlen($buf);
var_dump($done[0], $total, $pauses > 0);
echo $done[1] < 2.0 ? "resumed promptly\n" : sprintf("resumed late (%.3fs)\n", $done[1]);
CHILD);
$so = realpath(ini_get('extension_dir') . '/phasync.so');
// The child is killed after 20s, so a hang fails instead of blocking the run.
$p = proc_open([PHP_BINARY, '-d', 'extension=' . $so, $child], [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $cp);
stream_set_blocking($cp[1], false);
$out = ''; $deadline = microtime(true) + 20;
while (!feof($cp[1]) && microtime(true) < $deadline) {
    $rd = [$cp[1]]; $w = $e = null;
    if (stream_select($rd, $w, $e, 0, 200000)) $out .= fread($cp[1], 65536);
}
if (!feof($cp[1])) { proc_terminate($p, 9); $out .= "HUNG\n"; }
echo $out;
fclose($cp[1]); proc_close($p);
unlink($child);
proc_terminate($srv);
fclose($pipes[1]);
proc_close($srv);
?>
--EXPECT--
int(0)
int(4194304)
bool(true)
resumed promptly
