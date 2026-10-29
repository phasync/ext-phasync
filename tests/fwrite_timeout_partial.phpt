--TEST--
fwrite() on a blocking socket that times out after a partial write returns what was written after one timeout, with PHP's one notice, as natively
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_OS_FAMILY === "Darwin") die("skip natively a blocking fwrite() to a peer that never reads blocks in send() past the stream timeout on macOS, so there is nothing to match"); ?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
function observe(): array
{
    $server = stream_socket_server('tcp://127.0.0.1:0');
    $client = stream_socket_client('tcp://' . stream_socket_get_name($server, false));
    $peer   = stream_socket_accept($server);   // never reads
    stream_set_timeout($client, 0, 300000);
    $notices = [];
    set_error_handler(function (int $code, string $message) use (&$notices) {
        $notices[] = preg_replace(['/\d+ bytes/', '/errno=\d+/'], ['N bytes', 'errno=N'], $message);
        return true;
    });
    $t = microtime(true);
    $n = fwrite($client, str_repeat('x', 64 << 20));
    $elapsed = microtime(true) - $t;
    restore_error_handler();
    return [
        'partial'   => is_int($n) && $n > 0 && $n < 64 << 20,
        'timed_out' => stream_get_meta_data($client)['timed_out'],
        'notices'   => $notices,
        'one wait'  => $elapsed > 0.25 && $elapsed < 0.55,
    ];
}
$native = observe();
$seen = null;
(new Loop)->runAll(function () use (&$seen) { $seen = observe(); });
var_dump($seen === $native);
var_dump($seen);
?>
--EXPECT--
bool(true)
array(4) {
  ["partial"]=>
  bool(true)
  ["timed_out"]=>
  bool(true)
  ["notices"]=>
  array(1) {
    [0]=>
    string(78) "fwrite(): Send of N bytes failed with errno=N Resource temporarily unavailable"
  }
  ["one wait"]=>
  bool(true)
}
