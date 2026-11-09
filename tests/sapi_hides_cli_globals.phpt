--TEST--
sapi(): STDIN/STDOUT/STDERR and $argc/$argv (the CLI's own, visible to every Scope since they
are plain globals, not superglobals) are hidden inside a request and restored, unchanged,
once it ends -- PrestaShop's Tools::isPHPCLI()/argvToGET() otherwise treat it as a CLI run
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

// Separate call sites for "inside a request" vs "outside any request" -- a literal
// defined('STDIN') compiles to an opcode that caches its *first* true resolution forever
// (PHP assumes constants are add-only), so the same call site must stay on one side of the
// boundary, exactly as real app code (isPHPCLI() and the like) does.
function insideState(): string
{
    global $argc, $argv;

    return (\defined('STDIN') ? 'def' : 'undef') . ' argc=' . (isset($argc) ? 'set' : 'unset') . ' argv=' . (isset($argv) ? 'set' : 'unset');
}
function outsideState(): string
{
    global $argc, $argv;

    return (\defined('STDIN') ? 'def' : 'undef') . ' argc=' . (isset($argc) ? 'set' : 'unset') . ' argv=' . (isset($argv) ? 'set' : 'unset');
}

echo "before: ", outsideState(), "\n";

// Two separate requests: insideState() must come back hidden every time, not just the first.
for ($i = 0; $i < 2; $i++) {
    $a = new Sink;
    $a->info = ['method' => 'GET', 'query_string' => ''];
    sapi(function () {
        echo insideState();
    }, $a);
    echo "inside $i: ", $a->out, "\n";
}

echo "after: ", outsideState(), "\n";

// Interleaved: a fiber outside any request still sees them while one inside doesn't, and
// switching back and forth never leaks one state into the other.
$b = new Sink;
$b->info = ['method' => 'GET', 'query_string' => ''];
$seen = [];
$fiber = new Fiber(function () use ($b, &$seen) {
    sapi(function () use (&$seen) {
        $seen[] = 'fiber in 1: ' . insideState();
        Fiber::suspend();
        $seen[] = 'fiber in 2: ' . insideState();
    }, $b);
});
$fiber->start();
$seen[] = 'worker between: ' . outsideState();
$fiber->resume();
echo implode("\n", $seen), "\n";
echo "after fibers: ", outsideState(), "\n";
?>
--EXPECTF--
before: def argc=set argv=set
inside 0: undef argc=unset argv=unset
inside 1: undef argc=unset argv=unset
after: def argc=set argv=set
fiber in 1: undef argc=unset argv=unset
worker between: def argc=set argv=set
fiber in 2: undef argc=unset argv=unset
after fibers: def argc=set argv=set
