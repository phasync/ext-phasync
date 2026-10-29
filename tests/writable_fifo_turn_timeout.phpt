--TEST--
A blocking write that times out while waiting for the stream's write turn behaves as a write timeout: false, timed_out, PHP's notice
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
$server = stream_socket_server('tcp://127.0.0.1:0');
$client = stream_socket_client('tcp://' . stream_socket_get_name($server, false));
$peer   = stream_socket_accept($server);   // never reads
stream_set_timeout($client, 0, 300000);
$log = [];
set_error_handler(function (int $code, string $message) use (&$log) {
    $log[] = 'notice: ' . preg_replace(['/\d+ bytes/', '/errno=\d+/'], ['N bytes', 'errno=N'], $message);
    return true;
});
(new Loop)->runAll(
    function () use ($client, &$log) {
        $n = fwrite($client, str_repeat('a', 64 << 20));   // holds the turn until its timeout
        $log[] = 'first: ' . (is_int($n) && $n > 0 ? 'partial' : var_export($n, true));
    },
    function () use ($client, &$log) {
        stream_set_timeout($client, 0, 100000);            // shorter than the first write's wait
        $n = fwrite($client, "b\n");                       // waits for the turn, and times out there
        $log[] = 'second: ' . var_export($n, true) . ', timed_out: ' . var_export(stream_get_meta_data($client)['timed_out'], true);
    },
);
echo implode("\n", $log), "\n";
?>
--EXPECT--
notice: fwrite(): Send of N bytes failed with errno=N Resource temporarily unavailable
second: false, timed_out: true
notice: fwrite(): Send of N bytes failed with errno=N Resource temporarily unavailable
first: partial
