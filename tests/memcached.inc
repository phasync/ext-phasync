<?php
// Helpers for the memcached tests. PHASYNC_TEST_MEMCACHED=host:port[:/path/to/memcached.so]
// names a running memcached (and, when php-memcached is not in the normal
// extension_dir, the extension to load).

/** Parent side: run $body in a child php with phasync and php-memcached loaded. */
function memc_child(string $body): void
{
    [$host, $port, $so] = explode(':', getenv('PHASYNC_TEST_MEMCACHED')) + [2 => 'memcached'];
    $child = tempnam(sys_get_temp_dir(), 'phasync_mc_') . '.php';
    file_put_contents($child, '<?php
require ' . var_export(__DIR__ . '/loop.inc', true) . ';
require ' . var_export(__DIR__ . '/memcached.inc', true) . ';
const MEMC_SERVER = ' . var_export("$host:$port", true) . ';
' . $body);
    $phasync = realpath(ini_get('extension_dir') . '/phasync.so');
    passthru(escapeshellarg(PHP_BINARY) . ' -d extension=' . escapeshellarg($phasync)
        . ' -d extension=' . escapeshellarg($so) . ' ' . escapeshellarg($child) . ' 2>&1');
    unlink($child);
}

/** Child side: a proxy in front of the memcached that delays every reply. [port, process]. */
function memc_proxy(int $delayMs): array
{
    $proc = proc_open([PHP_BINARY, __DIR__ . '/memcached_proxy.inc', MEMC_SERVER, (string) $delayMs],
        [1 => ['pipe', 'w']], $pipes);
    register_shutdown_function(fn() => proc_terminate($proc));   // or the process's end waits for it
    return [(int) trim(fgets($pipes[1])), $proc];
}

/** Child side: a Memcached on these ports of 127.0.0.1. */
function memc_on(int ...$ports): Memcached
{
    $m = new Memcached;
    $m->setOption(Memcached::OPT_COMPRESSION, false);   // append()/prepend() refuse it
    foreach ($ports as $port) {
        $m->addServer('127.0.0.1', $port);
    }
    return $m;
}
