--TEST--
PostgreSQL: a coroutine entering a connection another one is in a call on gets a LogicException; the call is undisturbed
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
function race(string $name, Closure $first, Closure $second): void
{
    $log = [];
    (new Loop)->runAll(
        function () use ($first, &$log) { $log[] = 'first: ' . $first(); },
        function () use ($second, &$log) {
            try { $log[] = 'second: ' . var_export($second(), true); }
            catch (LogicException $e) { $log[] = 'second: ' . get_class($e) . ': ' . $e->getMessage(); }
        },
    );
    echo "$name\n  ", implode("\n  ", $log), "\n";
}
$sleep = "SELECT pg_sleep(0.3), 'done'";
$c = pg_connect(pg_dsn());
$r = pg_query($c, 'SELECT 1');
race('pg_query, pg_query', fn() => pg_fetch_row(pg_query($c, $sleep))[1], fn() => pg_query($c, 'SELECT 1'));
race('pg_query, pg_fetch_row on an earlier result', fn() => pg_fetch_row(pg_query($c, $sleep))[1], fn() => pg_fetch_row($r));
race('pg_query, pg_close', fn() => pg_fetch_row(pg_query($c, $sleep))[1], fn() => pg_close($c));
race('default link, explicit', fn() => pg_fetch_row(pg_query($sleep))[1], fn() => pg_query($c, 'SELECT 1'));
race('pg_query, pg_connect', fn() => pg_fetch_row(pg_query($c, $sleep))[1], fn() => pg_connect(pg_dsn()) instanceof PgSql\Connection);
race('pg_pconnect, pg_pconnect', fn() => pg_fetch_row(pg_query(pg_pconnect(pg_dsn()), $sleep))[1], fn() => pg_pconnect(pg_dsn()));
race('persistent, reused', fn() => pg_fetch_row(pg_query(pg_pconnect(pg_dsn()), $sleep))[1], fn() => pg_pconnect(pg_dsn()));
echo 'afterwards: ', pg_fetch_row(pg_query($c, 'SELECT 42'))[0], "\n";
$p = pg_pdo();
$s = $p->prepare('SELECT 1');
race('PDO query, PDO query', fn() => $p->query($sleep)->fetch(PDO::FETCH_NUM)[1], fn() => $p->query('SELECT 1'));
race('PDO query, statement execute', fn() => $p->query($sleep)->fetch(PDO::FETCH_NUM)[1], fn() => $s->execute());
race('two connections', fn() => $p->query($sleep)->fetch(PDO::FETCH_NUM)[1], fn() => pg_pdo()->query("SELECT pg_sleep(0.1), 'other'")->fetchColumn(1));
echo 'afterwards: ', $p->query('SELECT 42')->fetchColumn(), "\n";
CHILD, args: '-d ' . escapeshellarg('error_reporting=E_ALL & ~E_DEPRECATED')); // the default-link case is about concurrency, not its deprecation
?>
--EXPECT--
pg_query, pg_query
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
pg_query, pg_fetch_row on an earlier result
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
pg_query, pg_close
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
default link, explicit
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
pg_query, pg_connect
  second: true
  first: done
pg_pconnect, pg_pconnect
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
persistent, reused
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
afterwards: 42
PDO query, PDO query
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
PDO query, statement execute
  second: LogicException: Concurrent operations on the same resource are not supported
  first: done
two connections
  second: 'other'
  first: done
afterwards: 42
