--TEST--
Failsafe without the loop's help: a loop that keeps its Suspensions only by WeakReference drops them, so their managed fibers are destroyed (refcount, cycle collector) mid-wait; the extension settles what it held for them itself, and a dropped Suspension is never reported ready
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--INI--
phasync.fs_offload=all
--FILE--
<?php
use phasync\ext\Suspension;

// A loop that knows its Suspensions only by WeakReference: dropping them drops the
// fibers, as if phasync's teardown never ran. Live ones are polled and resumed.
final class RawLoop
{
    public array $weak = [];       // every Suspension it was given, by WeakReference
    public array $live = [];       // the ones a live reader holds on to

    public function catch(Closure $run, bool $keep): void
    {
        try {
            $run();
        } catch (Suspension $s) {
            $this->weak[] = WeakReference::create($s);
            if ($keep) {
                $this->live[] = $s;
            }
        }
    }
    /** Poll and resume for a while. */
    public function spin(float $seconds): void
    {
        $end = microtime(true) + $seconds;
        do {
            foreach (Suspension::poll($this->live, 0.01) as $k => $s) {
                unset($this->live[$k]);
                $this->catch(fn() => $s->resume(), true);
            }
        } while (microtime(true) < $end);
    }
}

function drop(RawLoop $loop, string $how, Closure $fn): void
{
    $holder = new stdClass;
    $f = Suspension::manage(new Fiber(function () use ($fn, $holder) {
        try { $fn(); } finally { echo "  finally ran\n"; }
    }));
    if ($how === 'gc') {
        $holder->f = $f;
    }
    $loop->catch(fn() => $f->start(), false);
    unset($f, $holder);
    gc_collect_cycles();
}

function start(RawLoop $loop, Closure $fn): void
{
    $f = Suspension::manage(new Fiber($fn));
    $loop->catch(fn() => $f->start(), true);
}

$file = tempnam(sys_get_temp_dir(), 'phasync_raw_');
file_put_contents($file, str_repeat('z', 1 << 24));
$loop = new RawLoop;
foreach (['refcount', 'gc'] as $how) {
    echo "socket read, then data and a close ($how)\n";
    [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
    drop($loop, $how, fn() => fread($a, 100));
    fwrite($b, 'x');
    $loop->spin(0.05);
    start($loop, function () use ($a) { echo "  then read: ", fread($a, 100), "\n"; });
    $loop->spin(0.05);
    fclose($a);
    fclose($b);

    echo "pool read, its thread finishing later ($how)\n";
    $fp = fopen($file, 'r');
    drop($loop, $how, fn() => fread($fp, 1 << 24));
    $loop->spin(0.3);
    fclose($fp);

    echo "sleep ($how)\n";
    drop($loop, $how, fn() => usleep(100000));
    $loop->spin(0.05);

    echo "closer dropped while a reader waits ($how)\n";
    [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
    start($loop, function () use ($a) { $r = fread($a, 100); echo "  reader: ", json_encode($r), "\n"; });
    drop($loop, $how, fn() => fclose($a));
    $loop->spin(0.05);                    // the reader leaves; poll() lets the held closer go
    gc_collect_cycles();
    echo "  stream: ", get_resource_type($a), "\n";
    fclose($b);
}
unlink($file);
?>
--EXPECT--
socket read, then data and a close (refcount)
  finally ran
  then read: x
pool read, its thread finishing later (refcount)
  finally ran
sleep (refcount)
  finally ran
closer dropped while a reader waits (refcount)
  reader: false
  finally ran
  stream: Unknown
socket read, then data and a close (gc)
  finally ran
  then read: x
pool read, its thread finishing later (gc)
  finally ran
sleep (gc)
  finally ran
closer dropped while a reader waits (gc)
  reader: false
  finally ran
  stream: Unknown
