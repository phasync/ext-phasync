--TEST--
mysqli and PDO mysql: the concurrent-use guard is released when the call fails (SQL error, exception, connection killed)
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
// The first coroutine's call fails; then another coroutine uses the connection.
function after(string $name, Closure $first, Closure $second, Closure ...$more): void
{
    $log = [];
    (new Loop)->runAll(
        function () use ($first, &$log) {
            try { $log[] = 'first: ' . var_export($first(), true); }
            catch (Throwable $e) { $log[] = 'first: ' . get_class($e); }
        },
        function () use ($second, &$log) {
            usleep(1500000);
            try { $log[] = 'second: ' . var_export($second(), true); }
            catch (Throwable $e) { $log[] = 'second: ' . get_class($e) . ': ' . $e->getMessage(); }
        },
        ...$more,
    );
    echo "$name\n  ", implode("\n  ", $log), "\n";
}
$db = $my();
after('mysqli: SQL error', fn() => $db->query('SELECT SLEEP(1), nope'), fn() => $db->query('SELECT 1')->fetch_row()[0]);
mysqli_report(MYSQLI_REPORT_OFF);
after('mysqli: SQL error, no exception', fn() => $db->query('SELECT SLEEP(1), nope'), fn() => $db->query('SELECT 2')->fetch_row()[0]);
after('mysqli: async SQL error', function () use ($db) {
    $db->query('SELECT SLEEP(1), nope', MYSQLI_ASYNC);
    $links = $errors = $reject = [$db];
    mysqli_poll($links, $errors, $reject, 5);
    return $db->reap_async_query();
}, fn() => $db->query('SELECT 3')->fetch_row()[0]);
mysqli_report(MYSQLI_REPORT_ERROR | MYSQLI_REPORT_STRICT);
$id = $db->thread_id;
after('mysqli: connection killed', fn() => $db->query('SELECT SLEEP(5), 4'), fn() => $db->query('SELECT 5'),
    function () use ($my, $id) { usleep(300000); $my()->query("KILL $id"); });
$pdo = new PDO("mysql:host=$host;port=$port", $user, $pass, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
after('PDO: SQL error', fn() => $pdo->query('SELECT SLEEP(1), nope'), fn() => $pdo->query('SELECT 6')->fetchColumn());
CHILD);
?>
--EXPECT--
mysqli: SQL error
  first: mysqli_sql_exception
  second: '1'
mysqli: SQL error, no exception
  first: false
  second: '2'
mysqli: async SQL error
  first: false
  second: '3'
mysqli: connection killed
  first: mysqli_sql_exception
  second: mysqli_sql_exception: MySQL server has gone away
PDO: SQL error
  first: PDOException
  second: 6
