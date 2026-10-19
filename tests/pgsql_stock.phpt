--TEST--
PostgreSQL (pg_* and PDO pgsql): results, errors and timeouts in a coroutine and outside one are stock PHP's
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
// The same scenario: without the extension, with it outside a coroutine, and in a coroutine.
$scenario = <<<'SCENARIO'
function scenario(): void
{
    $say = function (string $what, $v) { echo "# $what: ", is_string($v) ? trim($v) : var_export($v, true), "\n"; };
    $pdo = function (string $what, Closure $f) use ($say) {
        try { $say($what, $f()); } catch (PDOException $e) { $say($what, get_class($e) . ' ' . $e->getCode() . ' ' . $e->getMessage()); }
    };
    $say('refused', @pg_connect(pg_dsn('port=1')));
    $say('refused message', error_get_last()['message']);
    $say('bad password', @pg_connect(pg_dsn('password=wrong-' . getmypid())));
    $say('bad password message', error_get_last()['message']);
    $c = pg_connect(pg_dsn());
    $say('result', pg_fetch_all(pg_query($c, "SELECT 1 AS a, 'x' AS b UNION ALL SELECT 2, NULL")));
    $say('statement error', @pg_query($c, 'SELECT * FROM no_such_table'));
    $say('pg_last_error', pg_last_error($c));
    $say('notice', pg_query($c, "DO \$\$ BEGIN RAISE NOTICE 'hello'; END \$\$") !== false);
    $say('pg_last_notice', pg_last_notice($c));
    pg_query($c, 'SET statement_timeout = 100');
    $say('statement timeout', @pg_query($c, 'SELECT pg_sleep(2)'));
    $say('statement timeout error', pg_last_error($c));
    $say('after', pg_fetch_row(pg_query($c, 'SELECT 42'))[0]);
    pg_send_query($c, 'SELECT pg_sleep(0.05), 7');
    $say('async', pg_fetch_row(pg_get_result($c))[1]);
    $say('connection status', pg_connection_status($c) === PGSQL_CONNECTION_OK);
    $pdo('pdo refused', fn() => pg_pdo('port=1'));
    $pdo('pdo bad password', fn() => pg_pdo('password=wrong-' . getmypid()));
    $p = pg_pdo();
    $pdo('pdo result', fn() => $p->query("SELECT 1 AS a, 'x' AS b")->fetchAll(PDO::FETCH_ASSOC));
    $pdo('pdo statement error', fn() => $p->query('SELECT * FROM no_such_table'));
    $p->exec('SET statement_timeout = 100');
    $pdo('pdo statement timeout', fn() => $p->query('SELECT pg_sleep(2)'));
    $pdo('pdo prepared', fn() => ($s = $p->prepare('SELECT ?::int + 1')) && $s->execute([41]) ? $s->fetchColumn() : null);
}
SCENARIO;
$stock = db_child($scenario . 'scenario();', false);
$outside = db_child($scenario . 'scenario();');
$inside = db_child($scenario . '(new Loop)->runAll(scenario(...));');
echo "outside a coroutine: ", $outside === $stock ? "stock\n" : "differs:\n$outside\nstock:\n$stock\n";
echo "in a coroutine: ", $inside === $stock ? "stock\n" : "differs:\n$inside\nstock:\n$stock\n";
preg_match_all('/^# ([^:]+):/m', $stock, $m);
echo implode("\n", $m[1]), "\n";
?>
--EXPECT--
outside a coroutine: stock
in a coroutine: stock
refused
refused message
bad password
bad password message
result
statement error
pg_last_error
notice
pg_last_notice
statement timeout
statement timeout error
after
async
connection status
pdo refused
pdo bad password
pdo result
pdo statement error
pdo statement timeout
pdo prepared
