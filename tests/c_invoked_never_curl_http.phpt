--TEST--
sapi(): curl_exec() to a slow server lets other coroutines run, but its header, write and prereq callbacks run inline
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('curl')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
$server = '
$s = stream_socket_server("tcp://127.0.0.1:0");
$n = stream_socket_get_name($s, false); echo substr($n, strrpos($n, ":") + 1), "\n"; flush();
while ($c = @stream_socket_accept($s, 20)) {
    fread($c, 8192);
    usleep(300000);
    fwrite($c, "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    fclose($c);
}';
$srv = proc_open([PHP_BINARY, '-n', '-r', $server], [1 => ['pipe', 'w']], $pipes);
$port = trim(fgets($pipes[1]));
echo db_child('$port = ' . (int) $port . ";\nrequire " . var_export(__DIR__ . '/sink.inc', true) . ";\n" . <<<'CHILD'
// A is in curl_exec() (300 ms) while B ticks every 10 ms. A's callback sleeps 50 ms: B must not tick inside it.
foreach (['header' => CURLOPT_HEADERFUNCTION, 'write' => CURLOPT_WRITEFUNCTION, 'prereq' => CURLOPT_PREREQFUNCTION] as $kind => $opt) {
    $log = [];
    $done = false;
    (new Loop)->runAll(
        function () use ($opt, $port, &$log, &$done) {
            phasync\ext\sapi(function () use ($opt, $port, &$log, &$done) {
                $ch = curl_init("http://127.0.0.1:$port/");
                curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
                curl_setopt($ch, $opt, function ($ch, $x = null, $y = null) use (&$log, $opt) {
                    static $once;
                    if (!$once) { $once = true; $log[] = 'cb start'; usleep(50000); $log[] = 'cb end'; }
                    return $opt === CURLOPT_PREREQFUNCTION ? 0 : strlen((string) $x);
                });
                curl_exec($ch);
                $done = true;
            }, new Sink);
        },
        function () use (&$log, &$done) {
            phasync\ext\sapi(function () use (&$log, &$done) { while (!$done) { usleep(10000); $log[] = 'B'; } }, new Sink);
        },
    );
    $s = implode(',', $log);
    $inside = substr($s, strpos($s, 'cb start'), strpos($s, 'cb end') - strpos($s, 'cb start'));
    echo $kind, ': ', substr_count($s, 'B') >= 5 ? 'B ticked during the request' : 'B starved', ', ', str_contains($inside, 'B') ? 'B ran inside' : 'inline', "\n";
}
CHILD, true, '');
proc_terminate($srv);
?>
--EXPECT--
header: B ticked during the request, inline
write: B ticked during the request, inline
prereq: B ticked during the request, inline
