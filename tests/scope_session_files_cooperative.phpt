--TEST--
A Scope's session_start() on a session file another process has locked waits in its fiber, as outside a Scope: a new Scope keeps the live "files" save handler (ext-phasync's cooperative copy), not ext/session's own
--EXTENSIONS--
phasync
session
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
if (!function_exists('proc_open')) die('skip requires proc_open');
?>
--INI--
session.use_strict_mode=0
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
require __DIR__ . '/loop.inc';

$dir = sys_get_temp_dir() . '/phasync_scope_files_' . getmypid();
@mkdir($dir);
ini_set('session.save_path', $dir);
$file = "$dir/sess_scopelockid";
touch($file);

// Another process holds the session file's lock for 300ms.
$holder = proc_open([PHP_BINARY, '-n', '-r', '$f = fopen($argv[1], "c"); flock($f, LOCK_EX); echo "locked\n"; usleep(300000);', $file],
    [1 => ['pipe', 'w']], $pipes);
fgets($pipes[1]);

$loop = new Loop;
$ticks = 0;
$done = false;
$loop->go(function () use (&$done) {
    (new Scope)->run(function () {
        usleep(1000);   // a switch out and back: the Scope's session state is applied
        session_id('scopelockid');
        session_start();
        echo "the Scope has the session\n";
        session_write_close();
    });
    $done = true;
});
$loop->go(function () use (&$ticks, &$done) {
    while (!$done) { usleep(10000); $ticks++; }
});
$loop->run();
echo $ticks >= 10 ? "the process kept running\n" : "BLOCKED (ticks=$ticks)\n";

proc_close($holder);
array_map('unlink', glob("$dir/sess_*"));
rmdir($dir);
?>
--EXPECT--
the Scope has the session
the process kept running
