--TEST--
Poller: crosses from select() to epoll while streams are actively waited on, and stays on epoll
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

// A socketpair's registration is level-triggered and stays in the Poller
// after its wait resolves (poller.c: "a registration lives as long as its
// stream"), so waiting on 4200 of them in turn -- far below the switching
// threshold at any one time -- still grows the Poller's watched-fd count
// past it, crossing from select() to epoll partway through. The reader and
// writer run concurrently, so some waits are already parked when the switch
// happens, mid-flight, and must still be woken correctly afterward.
$n = 4200;
$pairs = [];
for ($i = 0; $i < $n; $i++) {
    $pairs[] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
}

$loop = new Loop;
$bad = 0;
$loop->runAll(
    function () use ($pairs, $loop, $n, &$bad) {
        foreach ($pairs as [$r, $w]) {
            $loop->poller->readable($r);
            if (fread($r, 1) !== 'x') {
                $bad++;
            }
        }
    },
    function () use ($pairs) {
        foreach ($pairs as [$r, $w]) {
            usleep(50);
            fwrite($w, 'x');
        }
    },
);
echo "crossed up, bad=$bad\n";

// Close all but the last 10 pairs: the watched-fd count falls far below the
// threshold; the Poller stays on epoll, and fresh waits still work.
foreach (array_slice($pairs, 0, $n - 10) as [$r, $w]) {
    fclose($r);
    fclose($w);
}
$tail = array_slice($pairs, $n - 10);
$bad2 = 0;
$loop->runAll(
    function () use ($tail, $loop, &$bad2) {
        foreach ($tail as [$r, $w]) {
            $loop->poller->readable($r);
            if (fread($r, 1) !== 'y') {
                $bad2++;
            }
        }
    },
    function () use ($tail) {
        foreach ($tail as [$r, $w]) {
            usleep(50);
            fwrite($w, 'y');
        }
    },
);
echo "after closes, bad=$bad2\n";
foreach ($tail as [$r, $w]) {
    fclose($r);
    fclose($w);
}
?>
--EXPECT--
crossed up, bad=0
after closes, bad=0
