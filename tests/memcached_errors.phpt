--TEST--
Memcached failures (server down, host not found) give the same errors inside a coroutine as outside
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists("Fiber")) die("skip requires Fibers");
if (!function_exists("proc_open")) die("skip requires proc_open");
if (!getenv("PHASYNC_TEST_MEMCACHED")) die("skip set PHASYNC_TEST_MEMCACHED=host:port[:/path/to/memcached.so]");
[$host, $port, $so] = explode(":", getenv("PHASYNC_TEST_MEMCACHED")) + [2 => "memcached"];
if (!@fsockopen($host, (int) $port)) die("skip no memcached on $host:$port");
exec(escapeshellarg(PHP_BINARY) . " -d extension=" . escapeshellarg($so) . " -r " . escapeshellarg("exit(class_exists(\"Memcached\") ? 0 : 1);") . " 2>&1", $o, $rc);
if ($rc) die("skip the memcached extension cannot be loaded");
?>
--FILE--
<?php
require __DIR__ . '/memcached.inc';
memc_child(<<<'CHILD'
// A port nobody listens on.
$s = stream_socket_server('tcp://127.0.0.1:0');
$dead = (int) parse_url('tcp://' . stream_socket_get_name($s, false), PHP_URL_PORT);
fclose($s);
// A server that accepts the connection (the kernel does) and never answers.
$stall = stream_socket_server('tcp://127.0.0.1:0');
$stalled = (int) parse_url('tcp://' . stream_socket_get_name($stall, false), PHP_URL_PORT);

function fail(string $host, int $port): array
{
    $m = new Memcached;
    $m->addServer($host, $port);
    $m->setOption(Memcached::OPT_CONNECT_TIMEOUT, 500);
    $m->setOption(Memcached::OPT_POLL_TIMEOUT, 200);
    return [$m->get('k'), $m->getResultCode(), $m->getResultMessage(), $m->set('k', 'v'), $m->getResultCode(), $m->getMulti(['a', 'b']), $m->getResultCode()];
}
foreach (['refused' => ['127.0.0.1', $dead], 'stalled' => ['127.0.0.1', $stalled], 'unresolvable' => ['no-such-host.invalid', 11211]] as $name => [$host, $port]) {
    $plain = fail($host, $port);
    $r = null;
    $loop = new Loop;
    $loop->runAll(function () use (&$r, $host, $port) { $r = fail($host, $port); });
    printf("%s: %s, same as outside a coroutine: %s, waited in the loop: %s\n", $name, json_encode(array_slice($plain, 0, 3)),
        var_export($r === $plain, true), var_export($loop->parks > 0, true));
}
CHILD);
?>
--EXPECTF--
refused: [false,%d,"%s"], same as outside a coroutine: true, waited in the loop: %s
stalled: [false,%d,"%s"], same as outside a coroutine: true, waited in the loop: true
unresolvable: [false,%d,"%s"], same as outside a coroutine: true, waited in the loop: true
