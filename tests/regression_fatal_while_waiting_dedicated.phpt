--TEST--
Regression: a fatal error in the loop while a coroutine waits on a dedicated thread (FIFO open()) leaves the thread to the extension, which cancels it at request shutdown
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
if (!function_exists('posix_mkfifo') && trim((string) @shell_exec('command -v mkfifo')) === '') die('skip needs posix_mkfifo or mkfifo(1)');
if (!is_readable('/proc/self/status')) die('skip requires /proc');
?>
--FILE--
<?php
// A FIFO open() runs on a dedicated thread holding a pointer to a task. A fatal
// error in the loop leaves the coroutine suspended in it, and the fiber is never
// resumed again: at request shutdown the extension orphans the task and cancels
// the thread (the task is the thread's to free). Observable from the next
// request (run_next()): only the main thread is left.
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/probe.php", '<?php
function threads(): int { preg_match("/^Threads:\\s+(\\d+)/m", file_get_contents("/proc/self/status"), $m); return (int) $m[1]; }
for ($i = 0; $i < 100 && threads() > 1; $i++) usleep(10000);   // cancelled, not waited for
echo "next request: threads=", threads(), "\n";
');
file_put_contents("$d/main.php", '<?php
require ' . var_export(__DIR__ . '/loop.inc', true) . ';
phasync\ext\run_next(__DIR__ . "/probe.php");
$fifo = __DIR__ . "/fifo";
posix_mkfifo($fifo, 0600);
register_shutdown_function(function () {
    preg_match("/^Threads:\\s+(\\d+)/m", file_get_contents("/proc/self/status"), $m);
    echo "after the fatal: threads=", $m[1], "\n";
});
$loop = new Loop;
$loop->onPark = function () {
    ini_set("memory_limit", "8M");
    $x = str_repeat("x", 32 << 20);            // fatal error in the loop
};
$loop->runAll(function () use ($fifo) {
    fopen($fifo, "r");                         // no writer: the thread blocks in open()
});
');
[$out, $status] = rn_exec($d, "$d/main.php", [], null, ['load' => ['posix']]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECTF--
%AFatal error: Allowed memory size of %d bytes exhausted%A
after the fatal: threads=2
next request: threads=1
status: 0
