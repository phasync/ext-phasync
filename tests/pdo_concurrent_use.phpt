--TEST--
PDO (any driver, subclasses too): a coroutine using a connection or its statements while another one is in a call on it (also a fetch of an unbuffered result) gets a LogicException
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if (!getenv('PHASYNC_TEST_MYSQL')) die('skip set PHASYNC_TEST_MYSQL=host:port:user:password');
if ($why = db_skip('pdo_mysql')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child(<<<'CHILD'
[$host, $port, $user, $pass] = explode(':', getenv('PHASYNC_TEST_MYSQL'));
class MyPDO extends PDO {}
function race(string $name, Closure $first, Closure $second): void
{
    $log = [];
    (new Loop)->runAll(
        function () use ($first, &$log) { $log[] = 'first: ' . var_export($first(), true); },
        function () use ($second, &$log) {
            usleep(200000);
            try { $log[] = 'second: ' . var_export($second(), true); }
            catch (LogicException $e) { $log[] = 'second: ' . get_class($e) . ': ' . $e->getMessage(); }
        },
    );
    echo "$name\n  ", implode("\n  ", $log), "\n";
}
$db = new MyPDO("mysql:host=$host;port=$port", $user, $pass, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$st = $db->prepare('SELECT 2');
race('subclass: statement while the connection is in a query', fn() => $db->query('SELECT SLEEP(1), 1')->fetchColumn(1), fn() => $st->execute());
race('subclass: connection while a statement executes', function () use ($db) {
    $s = $db->prepare('SELECT SLEEP(1), 3');
    $s->execute();
    return $s->fetchColumn(1);
}, fn() => $db->quote('x'));
$db->setAttribute(constant(defined('Pdo\\Mysql::ATTR_USE_BUFFERED_QUERY') ? 'Pdo\\Mysql::ATTR_USE_BUFFERED_QUERY' : 'PDO::MYSQL_ATTR_USE_BUFFERED_QUERY'), false);
race('unbuffered: fetch in flight', function () use ($db) {
    $s = $db->query('SELECT 4 UNION ALL SELECT SLEEP(1)');
    $rows = $s->fetchAll(PDO::FETCH_COLUMN);
    return $rows[0];
}, fn() => $db->query('SELECT 5'));
echo 'afterwards: ', $db->query('SELECT 6')->fetchColumn(), "\n";
CHILD);
?>
--EXPECT--
subclass: statement while the connection is in a query
  second: LogicException: Concurrent operations on the same resource are not supported
  first: 1
subclass: connection while a statement executes
  second: LogicException: Concurrent operations on the same resource are not supported
  first: 3
unbuffered: fetch in flight
  second: LogicException: Concurrent operations on the same resource are not supported
  first: 4
afterwards: 6
