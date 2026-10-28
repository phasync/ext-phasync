--TEST--
mysqli and PDO mysql: a coroutine using a connection another one is in a call on gets a LogicException; the call is undisturbed
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if (!getenv('PHASYNC_TEST_MYSQL')) die('skip set PHASYNC_TEST_MYSQL=host:port:user:password');
if ($why = db_skip('mysqli', 'pdo_mysql')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child(<<<'CHILD'
[$host, $port, $user, $pass] = explode(':', getenv('PHASYNC_TEST_MYSQL'));
mysqli_report(MYSQLI_REPORT_ERROR | MYSQLI_REPORT_STRICT);
$my = fn() => new mysqli($host, $user, $pass, '', (int) $port);
$pdo = fn() => new PDO("mysql:host=$host;port=$port", $user, $pass, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
// $first's call is in flight (SLEEP) while the second coroutine uses the connection.
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
$db = $my();
$other = $my();
race('mysqli: query', fn() => $db->query('SELECT SLEEP(1), 1')->fetch_row()[1], fn() => $db->query('SELECT 2')->fetch_row()[0]);
race('mysqli: function', fn() => mysqli_fetch_row(mysqli_query($db, 'SELECT SLEEP(1), 1'))[1], fn() => mysqli_query($db, 'SELECT 2'));
race('mysqli: close', fn() => $db->query('SELECT SLEEP(1), 1')->fetch_row()[1], fn() => $db->close());
race('mysqli: another connection', fn() => $db->query('SELECT SLEEP(1), 1')->fetch_row()[1], fn() => $other->query('SELECT 3')->fetch_row()[0]);
race('mysqli: async', function () use ($db) {
    $db->query('SELECT SLEEP(1), 5', MYSQLI_ASYNC);
    usleep(300000);
    $links = $errors = $reject = [$db];
    mysqli_poll($links, $errors, $reject, 5);
    return $db->reap_async_query()->fetch_row()[1];
}, fn() => $db->query('SELECT 4')->fetch_row()[0]);
$stmt = $other->prepare('SELECT SLEEP(1), 6');
race('mysqli: statement', function () use ($stmt) { $stmt->execute(); $stmt->bind_result($a, $b); $stmt->fetch(); return $b; }, fn() => $other->query('SELECT 7'));
echo 'afterwards: ', $my()->query('SELECT 8')->fetch_row()[0], "\n";
$db = $pdo();
$other = $pdo();
race('PDO: query', fn() => $db->query('SELECT SLEEP(1), 9')->fetchColumn(1), fn() => $db->query('SELECT 10')->fetchColumn());
race('PDO: another connection', fn() => $db->query('SELECT SLEEP(1), 9')->fetchColumn(1), fn() => $other->query('SELECT 11')->fetchColumn());
echo 'afterwards: ', $pdo()->query('SELECT 12')->fetchColumn(), "\n";
CHILD);
?>
--EXPECT--
mysqli: query
  second: LogicException: Concurrent operations on the same resource are not supported
  first: '1'
mysqli: function
  second: LogicException: Concurrent operations on the same resource are not supported
  first: '1'
mysqli: close
  second: LogicException: Concurrent operations on the same resource are not supported
  first: '1'
mysqli: another connection
  second: '3'
  first: '1'
mysqli: async
  second: LogicException: Concurrent operations on the same resource are not supported
  first: '5'
mysqli: statement
  second: LogicException: Concurrent operations on the same resource are not supported
  first: 6
afterwards: 8
PDO: query
  second: LogicException: Concurrent operations on the same resource are not supported
  first: 9
PDO: another connection
  second: 11
  first: 9
afterwards: 12
