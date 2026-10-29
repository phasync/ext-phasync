--TEST--
Poller: a regular file's wait, already parked under select(), survives a mid-flight switch to epoll
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
$lim = @shell_exec('bash -lc "ulimit -n" 2>/dev/null');
if ((int)$lim < 10000) die("skip needs ulimit -n >= 10000");
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';

// epoll_ctl() refuses a regular file (#29); select() always reports one
// ready. A regular-file wait started first -- while still well under the
// threshold -- must still be woken correctly if, before any poll() call, a
// burst of concurrent socket waits pushes the watched-fd count past the
// threshold and sweeps this already-parked registration into epoll.
$n = 4150;
$pairs = [];
for ($i = 0; $i < $n; $i++) {
    $pairs[] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
}

$log = [];
$bad = 0;
$coroutines = [
    function () use (&$log) {
        global $L;
        $L->poller->readable(fopen(__FILE__, 'r'));
        $log[] = 'regular file woke';
    },
];
foreach ($pairs as [$r, $w]) {
    $coroutines[] = function () use ($r, &$bad) {
        global $L;
        $L->poller->readable($r);
        if (fread($r, 1) !== 'x') {
            $bad++;
        }
    };
}
// Started last, once every waiter above has already parked (go() only queues
// them; none actually runs, and no poll() happens, until $L->run()).
$coroutines[] = function () use ($pairs) {
    foreach ($pairs as [$r, $w]) {
        fwrite($w, 'x');
    }
};

$L = new Loop;
$L->runAll(...$coroutines);
echo implode("\n", $log), "\n";
echo "bad=$bad\n";
?>
--EXPECT--
regular file woke
bad=0
