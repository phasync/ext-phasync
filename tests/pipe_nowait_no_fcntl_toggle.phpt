--TEST--
issue #42: a de-blocked pipe read/write uses preadv2()/pwritev2() RWF_NOWAIT, never toggling the fd's real O_NONBLOCK flag, on a kernel that supports it; a named FIFO that doesn't still works, via the fcntl fallback
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!is_dir('/proc/self/fd')) die('skip requires /proc'); ?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
require __DIR__ . '/fdflags.inc';
use function phasync\ext\io_stats;

function delta(array $before, array $after): array
{
    return [
        'pipe_nowait' => $after['pipe_nowait'] - $before['pipe_nowait'],
        'pipe_fcntl_fallback' => $after['pipe_fcntl_fallback'] - $before['pipe_fcntl_fallback'],
    ];
}

// ---- An anonymous pipe (proc_open()'s own kind -- what Scribunto's Lua
// child round trips actually use): a read that must park, then the write
// that wakes it, both inside a scope. The real O_NONBLOCK flag never moves,
// and io_stats() shows the RWF_NOWAIT path taken, not the toggle. ----
$p = proc_open('sh -c "sleep 0.2; printf hello"', [1 => ['pipe', 'w']], $pipes);
$fout = fd_for_stream($pipes[1]);

var_dump(real_nonblock($fout));
$before = io_stats();

$loop = new Loop;
$during = null;
$loop->runAll(
    function () use ($pipes) {
        echo "read: " . fread($pipes[1], 5) . "\n";
    },
    function () use ($fout, &$during) {
        $during = real_nonblock($fout);
    },
);
$d = delta($before, io_stats());
var_dump($during);
var_dump(real_nonblock($fout));
echo "anon pipe: nowait>0=" . ($d['pipe_nowait'] > 0 ? 'yes' : 'no') . " fcntl_fallback=" . $d['pipe_fcntl_fallback'] . "\n";

fclose($pipes[1]);
proc_close($p);

// ---- A named FIFO (tests/pool_fifo_rendezvous.phpt's own kind): this
// machine's kernel answers EOPNOTSUPP to RWF_NOWAIT for this one (confirmed
// independently of phasync, see this issue's own investigation notes) -- the
// read must still complete correctly, via the fcntl fallback. Whether THIS
// run's io_stats() shows the fallback counter move depends on whether an
// earlier test already taught the process this FIFO's device doesn't
// support RWF_NOWAIT (the cache is process-wide and never reset), so this
// only asserts correctness, not which path -- pipe_nowait_no_fcntl_toggle's
// sibling test (pool_fifo_rendezvous.phpt) already covers that it still
// works at all. ----
$fifo = sys_get_temp_dir() . '/phasync_test_fifo_' . getmypid();
@unlink($fifo);
if (function_exists('posix_mkfifo')) { posix_mkfifo($fifo, 0600); }
else { exec('mkfifo ' . escapeshellarg($fifo)); }

$data = null;
$loop2 = new Loop;
$loop2->runAll(
    function () use ($fifo, &$data) {
        $fh = fopen($fifo, 'r');
        $data = fread($fh, 100);
        fclose($fh);
    },
    function () use ($fifo) {
        $fh = fopen($fifo, 'w');
        fwrite($fh, "pong");
        fclose($fh);
    },
);
@unlink($fifo);
echo "fifo read: $data\n";
?>
--EXPECT--
bool(false)
read: hello
bool(false)
bool(false)
anon pipe: nowait>0=yes fcntl_fallback=0
fifo read: pong
