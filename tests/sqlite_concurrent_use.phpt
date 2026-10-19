--TEST--
SQLite (SQLite3 and PDO sqlite): a coroutine entering a connection another one is in a call on gets a LogicException; the call is undisturbed
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
// $db's call waits for $holder's lock; meanwhile another coroutine enters $db.
function race(string $name, $holder, $db, Closure $second): void
{
    $log = [];
    (new Loop)->runAll(
        function () use ($holder) { $holder->exec('BEGIN IMMEDIATE'); usleep(300000); $holder->exec('COMMIT'); },
        function () use ($db, &$log) { $db->exec('BEGIN IMMEDIATE'); $db->exec('INSERT INTO t VALUES (1)'); $db->exec('COMMIT'); $log[] = 'first: done'; },
        function () use ($second, &$log) {
            usleep(100000);
            try { $log[] = 'second: ' . var_export($second(), true); }
            catch (LogicException $e) { $log[] = 'second: ' . get_class($e) . ': ' . $e->getMessage(); }
        },
    );
    echo "$name\n  ", implode("\n  ", $log), "\n";
}
$open = function () use ($file) { $db = new SQLite3($file); $db->busyTimeout(5000); return $db; };
$holder = $open();
$db = $open();
$db->exec('CREATE TABLE t (x)');
$stmt = $db->prepare('SELECT count(*) FROM t');
$result = $db->query('SELECT 1');
race('SQLite3: query', $holder, $db, fn() => $db->querySingle('SELECT 1'));
race('SQLite3: statement', $holder, $db, fn() => $stmt->execute());
race('SQLite3: result', $holder, $db, fn() => $result->fetchArray(SQLITE3_NUM));
race('SQLite3: close', $holder, $db, fn() => $db->close());
race('SQLite3: another connection', $holder, $db, fn() => $open()->querySingle('SELECT count(*) > 0 FROM t'));
echo 'afterwards: ', $db->querySingle('SELECT count(*) FROM t'), "\n";
$open = fn() => new PDO("sqlite:$file", null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_TIMEOUT => 5]);
$holder = $open();
$db = $open();
$stmt = $db->prepare('SELECT count(*) FROM t');
race('PDO: query', $holder, $db, fn() => $db->query('SELECT 1'));
race('PDO: statement', $holder, $db, fn() => $stmt->execute());
race('PDO: another connection', $holder, $db, fn() => $open()->query('SELECT count(*) > 0 FROM t')->fetchColumn());
echo 'afterwards: ', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
unlink($file);
CHILD);
?>
--EXPECT--
SQLite3: query
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
SQLite3: statement
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
SQLite3: result
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
SQLite3: close
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
SQLite3: another connection
  second: 1
  first: done
afterwards: 5
PDO: query
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
PDO: statement
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
PDO: another connection
  second: 1
  first: done
afterwards: 8
