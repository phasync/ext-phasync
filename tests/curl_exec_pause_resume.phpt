--TEST--
curl_exec() in a coroutine: a WRITEFUNCTION that pauses (CURL_WRITEFUNC_PAUSE) resumes when another coroutine calls curl_pause(CONT)
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

$child = tempnam(sys_get_temp_dir(), 'phasync_cp_') . '.php';
file_put_contents($child, '<?php $port = ' . (int) $port . '; require ' . var_export(__DIR__ . '/loop.inc', true) . ';' . <<<'CHILD'

// A buffers in its WRITEFUNCTION and pauses above 256 KiB; B drains the buffer
// and resumes the transfer with curl_pause(CONT).
$ch = curl_init("http://127.0.0.1:$port/");
$buf = ''; $total = 0; $paused = false; $pauses = 0; $done = false; $r = null;
curl_setopt($ch, CURLOPT_WRITEFUNCTION, function ($ch, $data) use (&$buf, &$paused, &$pauses) {
    if (strlen($buf) >= 262144) { $paused = true; $pauses++; return CURL_WRITEFUNC_PAUSE; }
    $buf .= $data;
    return strlen($data);
});
(new Loop)->runAll(
    function () use ($ch, &$r, &$done) { $r = curl_exec($ch); $done = true; },
    function () use ($ch, &$buf, &$total, &$paused, &$done) {
        while (!$done) {
            usleep(5000);
            if ($paused) { $total += strlen($buf); $buf = ''; $paused = false; curl_pause($ch, CURLPAUSE_CONT); }
        }
    },
);
$total += strlen($buf);
var_dump($r, curl_errno($ch), $total, $pauses > 0);
CHILD);
$so = realpath(ini_get('extension_dir') . '/phasync.so');
// The child is killed after 15s, so a hang fails instead of blocking the run.
$p = proc_open([PHP_BINARY, '-d', 'extension=' . $so, $child], [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $cp);
stream_set_blocking($cp[1], false);
$out = ''; $deadline = microtime(true) + 15;
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
bool(true)
int(0)
int(4194304)
bool(true)
