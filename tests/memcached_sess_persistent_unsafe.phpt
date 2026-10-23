--TEST--
manage() refuses to run when memcached.sess_persistent=1 shares the session handle without the per-handle lock (#5)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!getenv("PHASYNC_TEST_MEMCACHED")) die("skip set PHASYNC_TEST_MEMCACHED=host:port[:/path/to/memcached.so] (no server contacted, just the extension)");
[, , $so] = explode(":", getenv("PHASYNC_TEST_MEMCACHED")) + [2 => "memcached"];
exec(escapeshellarg(PHP_BINARY) . " -d extension=" . escapeshellarg($so) . " -r " . escapeshellarg("exit(class_exists(\"Memcached\") ? 0 : 1);") . " 2>&1", $o, $rc);
if ($rc) die("skip the memcached extension cannot be loaded");
?>
--FILE--
<?php
[, , $so] = explode(":", getenv("PHASYNC_TEST_MEMCACHED")) + [2 => "memcached"];
$phasync = realpath(ini_get('extension_dir') . '/phasync.so');
$child = tempnam(sys_get_temp_dir(), 'phasync_mcs_') . '.php';
file_put_contents($child, '<?php
require ' . var_export(__DIR__ . '/loop.inc', true) . ';
try {
    (new Loop)->runAll(function () { echo "ran\n"; });
    echo "no exception\n";
} catch (\Throwable $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
// Safe once the dangerous combination is gone: another manage() in the same
// process still runs.
ini_set("memcached.sess_persistent", "0");
(new Loop)->runAll(function () { echo "ran after fixing the ini\n"; });
');
passthru(escapeshellarg(PHP_BINARY)
    . ' -d extension=' . escapeshellarg($phasync)
    . ' -d extension=' . escapeshellarg($so)
    . ' -d session.save_handler=memcached -d memcached.sess_persistent=1 '
    . escapeshellarg($child) . ' 2>&1');
unlink($child);
?>
--EXPECT--
LogicException: memcached.sess_persistent=1 with session.save_handler=memcached shares a persistent connection between coroutines without the per-handle lock, and its session lock wait blocks the process; manage() refuses to run with this combination -- set memcached.sess_persistent=0, or use another session handler (see README, Memcached)
ran after fixing the ini
