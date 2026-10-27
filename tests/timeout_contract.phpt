--TEST--
Timeout contract: a wait in a coroutine ends at the native timeout (none = forever) and finishes the op the native way
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
if (!function_exists('stream_socket_pair')) die('skip requires stream_socket_pair');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
require __DIR__ . '/timing.inc';
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
stream_set_timeout($a, 0, 300000);

// 1. A wait ends at the stream's own timeout: fgets() returns the partial line,
//    timed_out is set, and no exception escapes.
fwrite($b, 'partial');
$t = microtime(true);
(new Loop)->runAll(function () use ($a) {
    var_dump(fgets($a));                               // string(7) "partial"
    var_dump(stream_get_meta_data($a)['timed_out']);   // bool(true)
});
var_dump(in_range(microtime(true) - $t, 0.3));         // the stream's timeout

// 2. A new call starts a fresh, full timeout; fread() on an empty stream that
//    times out returns false.
$t = microtime(true);
(new Loop)->runAll(function () use ($a) {
    var_dump(fread($a, 100));                          // bool(false)
    var_dump(stream_get_meta_data($a)['timed_out']);   // bool(true)
});
var_dump(in_range(microtime(true) - $t, 0.3));         // the stream's timeout

// 3. Where native PHP would wait forever (a pipe), the wait has no timeout: it
//    ends at the child's exit.
ini_set('default_socket_timeout', '0');
$p = proc_open([PHP_BINARY, '-n', '-r', 'usleep(300000);'], [1 => ['pipe', 'w']], $pipes);
$t = microtime(true);
(new Loop)->runAll(fn() => var_dump(fread($pipes[1], 10)));  // string(0) ""
var_dump(microtime(true) - $t > 0.2);
fclose($pipes[1]); proc_close($p);

// 4. A timed-out write behaves like native: the "Send of N bytes failed" notice,
//    false, timed_out set.
[$c, $d] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
stream_set_blocking($c, false);
$chunk = str_repeat('x', 65536);
while (fwrite($c, $chunk) > 0) {}                       // fill the send buffer
stream_set_blocking($c, true);
stream_set_timeout($c, 0, 200000);
(new Loop)->runAll(function () use ($c) {
    var_dump(fwrite($c, 'y'));                         // notice + bool(false)
    var_dump(stream_get_meta_data($c)['timed_out']);   // bool(true)
});

fclose($a); fclose($b); fclose($c); fclose($d);
?>
--EXPECTF--
string(7) "partial"
bool(true)
bool(true)
bool(false)
bool(true)
bool(true)
string(0) ""
bool(true)

%s: fwrite(): Send of 1 bytes failed with errno=%d Resource temporarily unavailable in %s on line %d
bool(false)
bool(true)
