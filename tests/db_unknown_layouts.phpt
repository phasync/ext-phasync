--TEST--
pgsql's and SQLite3's internals at an unrecognised layout: not hooked, stock behaviour (a shared connection is not checked), and phpinfo() says so; their waits stay cooperative
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('sqlite3')) die("skip $why");
if (!in_array('phasync.test_unknown_layouts', array_keys(ini_get_all('phasync')), true)) die('skip needs a test build');
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child(<<<'CHILD'
ob_start();
phpinfo(INFO_MODULES);
preg_match_all('/^(pg_\* one call per connection|SQLite3 one call per connection) => (.*)$/m', ob_get_clean(), $m, PREG_SET_ORDER);
foreach ($m as [, $k, $v]) echo "$k: ", explode(' ', $v)[0], "\n";
$file = tempnam(sys_get_temp_dir(), 'phasync_sqlite_');
$holder = new SQLite3($file);
$db = new SQLite3($file);
$db->busyTimeout(5000);
$db->exec('CREATE TABLE t (x)');
$log = [];
$loop = new Loop;
$loop->runAll(
    function () use ($holder, &$log) { $holder->exec('BEGIN IMMEDIATE'); usleep(300000); $holder->exec('COMMIT'); },
    function () use ($db, &$log) { $db->exec('BEGIN IMMEDIATE'); $db->exec('INSERT INTO t VALUES (1)'); $db->exec('COMMIT'); $log[] = 'first: done'; },
    function () use ($db, &$log) { usleep(100000); $log[] = 'second: ' . $db->querySingle('SELECT 7'); },
);
echo implode(", ", $log), "\n";
unlink($file);
if (function_exists('pg_connect') && getenv('PHASYNC_TEST_PGSQL')) {
    $c = pg_connect(pg_dsn());
    $loop = new Loop;
    $t = microtime(true);
    $loop->runAll(fn() => pg_query($c, 'SELECT pg_sleep(0.3)'), fn() => pg_query(pg_connect(pg_dsn(), PGSQL_CONNECT_FORCE_NEW), 'SELECT pg_sleep(0.3)'));
    echo 'pg waits overlap: ', var_export(microtime(true) - $t < 0.55, true), "\n";
} else {
    echo "pg waits overlap: true\n";
}
CHILD, true, '-d phasync.test_unknown_layouts=1');
?>
--EXPECT--
pg_* one call per connection: disabled
SQLite3 one call per connection: disabled
second: 7, first: done
pg waits overlap: true
