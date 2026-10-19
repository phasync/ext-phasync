--TEST--
PostgreSQL over TLS: the handshake and queries suspend the coroutine
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!getenv('PHASYNC_TEST_PGSQL')) die('skip set PHASYNC_TEST_PGSQL to a libpq conninfo string');
require __DIR__ . '/db.inc';
if ($why = db_skip('pgsql', 'pdo_pgsql')) die("skip $why");
if (trim(db_child('echo @pg_connect(pg_dsn("sslmode=require")) ? "ok" : "no";', false)) !== 'ok') die('skip the server does not take TLS');
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child(<<<'CHILD'
$log = [];
$loop = new Loop;
$t = microtime(true);
$tls = fn($c) => pg_fetch_row(pg_query($c, "SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()"))[0];
$loop->runAll(
    function () use (&$log, $tls) { $c = pg_connect(pg_dsn('sslmode=require')); pg_query($c, 'SELECT pg_sleep(0.5)'); $log[] = 'A tls=' . $tls($c); },
    function () use (&$log) {
        $p = pg_pdo('sslmode=require');
        $p->query('SELECT pg_sleep(0.5)');
        $log[] = 'B tls=' . $p->query('SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()')->fetchColumn();
    },
);
sort($log);
printf("%s, overlapped: %s, parks: %s\n", implode(', ', $log), var_export(microtime(true) - $t < 0.9, true), var_export($loop->parks > 2, true));
CHILD);
?>
--EXPECT--
A tls=t, B tls=1, overlapped: true, parks: true
