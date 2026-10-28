--TEST--
A fiber thrown into or resumed directly (Fiber::throw()/resume(), not through its Suspension): the cancelled call reports nothing, not even to a user error handler; poll() then reports the Suspension, whose resume() does nothing
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (trim((string) @shell_exec('command -v mkfifo')) === '') die('skip needs mkfifo(1)'); ?>
--FILE--
<?php
use phasync\ext\Suspension;

set_error_handler(function (int $no, string $msg) {
    echo "  handler: $msg\n";
    throw new ErrorException($msg, 0, $no);   // as frameworks do
});

echo "-- Fiber::throw() into a FIFO open() waiting on its thread\n";
$fifo = sys_get_temp_dir() . '/phasync_cont_' . getmypid();
exec('mkfifo ' . escapeshellarg($fifo));
$f = Suspension::manage(new Fiber(function () use ($fifo) {
    try {
        fopen($fifo, 'r');
    } catch (Throwable $e) {
        echo "  fiber caught ", get_class($e), ": ", $e->getMessage(), "\n";
    } finally {
        try { fopen('/nonexistent/x', 'r'); } catch (ErrorException $e) { echo "  finally's own warning reached the handler\n"; }
    }
}));
try { $f->start(); } catch (Suspension $s) {}
$f->throw(new RuntimeException('stop'));
var_dump($f->isTerminated());

echo "-- the loop's Suspension: suspended, and now it isn't\n";
var_dump(Suspension::poll(['k' => $s], -1) === ['k' => $s], $s->isReady());
var_dump($s->resume());
try { $s->resume(); } catch (LogicException $e) { echo "  again: ", $e->getMessage(), "\n"; }
var_dump(Suspension::poll([$s], 0), $s->isReady());

echo "-- Fiber::resume() of a ready wait\n";
$f = Suspension::manage(new Fiber(function () { usleep(1000); echo "  fiber ran on\n"; return 'done'; }));
try { $f->start(); } catch (Suspension $s) {}
while (!Suspension::poll([$s], -1));
$f->resume();
var_dump($f->getReturn(), Suspension::poll([$s], 0) === [$s], $s->resume(), $s->isReady());

echo "-- errors later are reported as usual\n";
try { fopen('/nonexistent/y', 'r'); } catch (ErrorException $e) { echo "  caught it\n"; }
unlink($fifo);
?>
--EXPECTF--
-- Fiber::throw() into a FIFO open() waiting on its thread
  fiber caught RuntimeException: stop
  handler: fopen(/nonexistent/x): Failed to open stream: No such file or directory
  finally's own warning reached the handler
bool(true)
-- the loop's Suspension: suspended, and now it isn't
bool(true)
bool(true)
NULL
  again: This Suspension was already resumed
array(0) {
}
bool(false)
-- Fiber::resume() of a ready wait
  fiber ran on
string(4) "done"
bool(true)
NULL
bool(false)
-- errors later are reported as usual
  handler: fopen(/nonexistent/y): Failed to open stream: No such file or directory
  caught it
