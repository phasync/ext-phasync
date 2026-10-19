--TEST--
Poller::writable() on one descriptor isn't blocked by another coroutine's readable() wait reached through foreach() over a Traversable (#108)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';

// valid() is called by the engine's FE_FETCH_R opcode, not through a DO_FCALL:
// phasync_from_c() must still recognize this as a normal coroutine wait, not
// run it inline with a real blocking poll(2) that would freeze every coroutine.
class NeverReadyIterator implements Iterator
{
    public function __construct(private Loop $loop, private $sock) {}
    public function rewind(): void {}
    public function valid(): bool
    {
        try {
            $this->loop->poller->readable($this->sock, 1.0);
        } catch (LoopTimeout) {
        }
        return false;
    }
    public function current(): mixed { return null; }
    public function key(): mixed { return 0; }
    public function next(): void {}
}

// Both peers are kept open: closing one would make its sibling spuriously
// readable (EOF), defeating the "never becomes ready" setup below.
[$a, $aPeer] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);   // never written to
[$c, $cPeer] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);   // writable at once

$loop = new Loop;
$log = [];
$t0 = microtime(true);
$tA = $tB = null;
$loop->runAll(
    function () use ($loop, $a, &$log, &$tA, $t0) {
        foreach (new NeverReadyIterator($loop, $a) as $x) {
        }
        $tA = microtime(true) - $t0;
        $log[] = 'A done';
    },
    function () use ($loop, $c, &$log, &$tB, $t0) {
        $loop->poller->writable($c);
        $tB = microtime(true) - $t0;
        $log[] = 'B done';
    }
);
sort($log);
echo implode("\n", $log), "\n";
echo 'B finished well before A: ', var_export($tB < 0.3 && $tA >= 0.9, true), "\n";
fclose($a);
fclose($aPeer);
fclose($c);
fclose($cPeer);
?>
--EXPECT--
A done
B done
B finished well before A: true
