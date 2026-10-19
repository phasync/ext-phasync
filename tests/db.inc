<?php
// Helpers for the database tests. pgsql, pdo_pgsql, sqlite3 and pdo_sqlite are
// usually shared extensions run-tests' -n cannot load: the scenarios run in a
// child php with the normal ini. PHASYNC_TEST_PGSQL is a libpq conninfo string
// ("host=127.0.0.1 port=5432 user=postgres password=...").

/** Run $body in a child php, with phasync loaded (and these php arguments) unless $phasync is false; its output. */
function db_child(string $body, bool $phasync = true, string $args = ''): string
{
    $child = tempnam(sys_get_temp_dir(), 'phasync_db_') . '.php';
    file_put_contents($child, '<?php
require ' . var_export(__DIR__ . '/loop.inc', true) . ';
require ' . var_export(__DIR__ . '/db.inc', true) . ';
' . $body);
    $so = realpath(ini_get('extension_dir') . '/phasync.so');
    $out = shell_exec(escapeshellarg(PHP_BINARY) . ($phasync ? ' -d extension=' . escapeshellarg($so) . " $args" : '')
        . ' ' . escapeshellarg($child) . ' 2>&1');
    unlink($child);
    return (string) $out;
}

/** SKIPIF: why a child php cannot run the test, or null. */
function db_skip(string ...$extensions): ?string
{
    if (!function_exists('shell_exec')) {
        return 'requires shell_exec';
    }
    $missing = trim(db_child('foreach (' . var_export($extensions, true) . ' as $e) if (!extension_loaded($e)) echo "$e ";', false));
    return $missing === '' ? null : "the child php lacks $missing";
}

/** The conninfo with these settings added. */
function pg_dsn(string $extra = ''): string
{
    return trim(getenv('PHASYNC_TEST_PGSQL') . ' ' . $extra);
}

/** new PDO for the conninfo, with these settings added. */
function pg_pdo(string $extra = ''): PDO
{
    $kv = [];
    foreach (preg_split('/\s+/', pg_dsn($extra)) as $pair) {
        [$k, $v] = explode('=', $pair, 2);
        $kv[$k] = $v;
    }
    $user = $kv['user'] ?? null;
    $pass = $kv['password'] ?? null;
    unset($kv['user'], $kv['password']);
    $dsn = implode(';', array_map(fn($k, $v) => "$k=$v", array_keys($kv), $kv));
    return new PDO("pgsql:$dsn", $user, $pass, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
}
