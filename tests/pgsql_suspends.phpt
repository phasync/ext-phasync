--TEST--
PostgreSQL (pg_* and PDO pgsql): connecting and queries suspend the coroutine; two connections' waits overlap
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!getenv('PHASYNC_TEST_PGSQL')) die('skip set PHASYNC_TEST_PGSQL to a libpq conninfo string');
require __DIR__ . '/db.inc';
if ($why = db_skip('pgsql', 'pdo_pgsql')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child(<<<'CHILD'
$q = fn($c, $n) => pg_fetch_row(pg_query($c, "SELECT pg_sleep(0.5), $n"))[1];
$cases = [
    'pg_*' => fn($n) => $q(pg_connect(pg_dsn()), $n),
    'PDO'  => fn($n) => pg_pdo()->query("SELECT pg_sleep(0.5), $n")->fetch(PDO::FETCH_NUM)[1],
    'pg_* with sslmode=prefer' => fn($n) => $q(pg_connect(pg_dsn('sslmode=prefer')), $n),
];
foreach ($cases as $name => $run) {
    $log = [];
    $loop = new Loop;
    $t = microtime(true);
    $loop->runAll(
        function () use ($run, &$log) { $log[] = 'A:' . $run(1); },
        function () use ($run, &$log) { $log[] = 'B:' . $run(2); },
        function () use (&$log) { for ($i = 0; $i < 3; $i++) { usleep(100000); $log[] = 't'; } },
    );
    $wall = microtime(true) - $t;
    sort($log);
    printf("%s: %s, overlapped: %s\n", $name, implode(' ', $log), var_export($wall < 0.9, true));
}
CHILD);
?>
--EXPECT--
pg_*: A:1 B:2 t t t, overlapped: true
PDO: A:1 B:2 t t t, overlapped: true
pg_* with sslmode=prefer: A:1 B:2 t t t, overlapped: true
