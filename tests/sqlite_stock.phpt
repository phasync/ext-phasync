--TEST--
SQLite (SQLite3 and PDO sqlite): results, errors and busy timeouts in a coroutine and outside one are stock PHP's
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
// The same scenario: without the extension, with it outside a coroutine, and in a coroutine.
$scenario = <<<'SCENARIO'
function scenario(): void
{
    $say = function (string $what, $v) { echo "# $what: ", is_string($v) ? trim($v) : var_export($v, true), "\n"; };
    $try = function (string $what, Closure $f) use ($say) {
        try { $say($what, $f()); } catch (Exception $e) { $say($what, get_class($e) . ' ' . $e->getCode() . ' ' . $e->getMessage()); }
    };
    $file = sys_get_temp_dir() . '/phasync_sqlite_stock_' . getmypid() . '.db';
    @unlink($file);
    $try('open in a missing directory', fn() => new SQLite3('/nonexistent/dir/x.db'));
    $try('pdo open in a missing directory', fn() => new PDO('sqlite:/nonexistent/dir/x.db'));
    $a = new SQLite3($file);
    $say('create', $a->exec('CREATE TABLE t (x INTEGER PRIMARY KEY, y TEXT NOT NULL)'));
    $say('insert', $a->exec("INSERT INTO t VALUES (1, 'one'), (2, 'two')"));
    $say('constraint', @$a->exec('INSERT INTO t VALUES (1, NULL)'));
    $say('constraint error', $a->lastErrorCode() . ' ' . $a->lastErrorMsg());
    $say('syntax', @$a->query('SELEKT 1'));
    $say('syntax error', $a->lastErrorCode() . ' ' . $a->lastErrorMsg());
    $r = $a->query('SELECT * FROM t ORDER BY x');
    $rows = [];
    while ($row = $r->fetchArray(SQLITE3_ASSOC)) { $rows[] = $row; }
    $say('rows', $rows);
    $b = new SQLite3($file);
    $b->busyTimeout(200);
    $a->exec('BEGIN IMMEDIATE');
    $t = microtime(true);
    $say('busy', @$b->exec('BEGIN IMMEDIATE'));
    $say('busy error', $b->lastErrorCode() . ' ' . $b->lastErrorMsg());
    $say('busy waited the timeout', microtime(true) - $t >= 0.19);
    $a->exec('COMMIT');
    $say('after the commit', $b->exec('BEGIN IMMEDIATE') && $b->exec('COMMIT'));
    $p = new PDO("sqlite:$file", null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_TIMEOUT => 1]);
    $q = new PDO("sqlite:$file", null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $q->setAttribute(PDO::ATTR_TIMEOUT, 1);
    $try('pdo rows', fn() => $p->query('SELECT * FROM t ORDER BY x')->fetchAll(PDO::FETCH_NUM));
    $try('pdo constraint', fn() => $p->exec('INSERT INTO t VALUES (1, NULL)'));
    $try('pdo syntax', fn() => $p->query('SELEKT 1'));
    $p->exec('BEGIN IMMEDIATE');
    $try('pdo busy', fn() => $q->exec('BEGIN IMMEDIATE'));
    $p->exec('COMMIT');
    $try('pdo prepared', fn() => ($s = $q->prepare('SELECT y FROM t WHERE x = ?')) && $s->execute([2]) ? $s->fetchColumn() : null);
    unset($a, $b, $p, $q, $r, $s);
    unlink($file);
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
open in a missing directory
pdo open in a missing directory
create
insert
constraint
constraint error
syntax
syntax error
rows
busy
busy error
busy waited the timeout
after the commit
pdo rows
pdo constraint
pdo syntax
pdo busy
pdo prepared
