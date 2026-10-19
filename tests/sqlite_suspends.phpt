--TEST--
SQLite (SQLite3 and PDO sqlite): a connection waiting for another's lock waits through the loop and proceeds after the commit; fsync runs on the pool
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('sqlite3', 'pdo_sqlite')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child(<<<'CHILD'
$file = tempnam(sys_get_temp_dir(), 'phasync_sqlite_');
$open = [
    'SQLite3' => function () use ($file) { $db = new SQLite3($file); $db->enableExceptions(true); $db->busyTimeout(5000); return $db; },
    'PDO' => fn() => new PDO("sqlite:$file", null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_TIMEOUT => 5]),
];
foreach ($open as $name => $connect) {
    @unlink($file);
    $a = $connect();
    $b = $connect();
    $a->exec('CREATE TABLE t (x)');
    $log = [];
    $loop = new Loop;
    $t = microtime(true);
    $loop->runAll(
        function () use ($a, &$log) {
            $a->exec('BEGIN IMMEDIATE');
            $a->exec('INSERT INTO t VALUES (1)');
            $log[] = 'A holds the lock';
            usleep(300000);
            $a->exec('COMMIT');
            $log[] = 'A committed';
        },
        function () use ($b, &$log) {
            $b->exec('BEGIN IMMEDIATE');
            $log[] = 'B has the lock';
            $b->exec('INSERT INTO t VALUES (2)');
            $b->exec('COMMIT');
        },
        function () use (&$log) { for ($i = 0; $i < 2; $i++) { usleep(100000); $log[] = 'tick'; } },
    );
    $rows = $name === 'PDO' ? $a->query('SELECT group_concat(x) FROM t')->fetchColumn() : $a->querySingle('SELECT group_concat(x) FROM t');
    printf("%s: %s; rows %s; waited %s\n", $name, implode(', ', $log), $rows, var_export(microtime(true) - $t >= 0.3, true));
}
unlink($file);
CHILD);
?>
--EXPECT--
SQLite3: A holds the lock, tick, tick, A committed, B has the lock; rows 1,2; waited true
PDO: A holds the lock, tick, tick, A committed, B has the lock; rows 1,2; waited true
