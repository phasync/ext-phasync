<?php

function spin(float $s): void { $t = microtime(true); while (microtime(true) - $t < $s); }
function threads(): int { clearstatcache(true); return count(scandir('/proc/self/task')) - 2; }   // after fork(), /proc/self must not come from the realpath cache (php/php-src#23986)
// pthread_join() returns when the kernel clears the thread's tid, a moment before its
// /proc/self/task entry goes: give the count up to a second to settle.
function threads_back_to(int $n): bool { for ($i = 0; $i < 100 && threads() !== $n; $i++) usleep(10000); return threads() === $n; }

$base = threads();
$n = 0;
preempt_handler(function () use (&$n) { $n++; }, 0.05);

$pid = pcntl_fork();
if ($pid === 0) {
    $n = 0;
    spin(0.5);
    echo "child: ", $n >= 4 && $n <= 10 ? "ok" : "calls: $n", ", threads: ", threads() - $base, "\n";
    var_dump(preempt_handler(null) instanceof Closure, threads_back_to($base));
    exit(0);
}
pcntl_waitpid($pid, $status);
var_dump(pcntl_wexitstatus($status));
$n = 0;
spin(0.5);
echo "parent: ", $n >= 4 && $n <= 10 ? "ok" : "calls: $n", ", threads: ", threads() - $base, "\n";

echo "-- none set: the child has no timer\n";
preempt_handler(null);
$pid = pcntl_fork();
if ($pid === 0) {
    echo "child threads: ", threads() - $base, "\n";
    exit(0);
}
pcntl_waitpid($pid, $status);
?>
