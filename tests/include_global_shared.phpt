--TEST--
include_global() throws instead of sharing a variable with another include_global() still running in a Fiber
--EXTENSIONS--
phasync
--FILE--
<?php
function run(): void
{
    $file = __DIR__ . '/include_global_shared.inc';
    // a second file running in the global scope while the first is suspended in its Fiber
    $a = new Fiber(fn () => phasync\ext\include_global($file));
    $a->start();
    try {
        phasync\ext\include_global($file);
    } catch (Error $e) {
        echo $e->getMessage() === "phasync\\ext\\include_global(): \$shared is a top-level variable of code still running in the global scope, which $file cannot share" ? 'concurrent: refused' : $e->getMessage(), "\n";
    }
    $a->resume();
    echo "first returned ", $a->getReturn(), ", global ", $GLOBALS['shared'], "\n";
    // once it ended, the same file runs again and sees the global it left
    $b = new Fiber(fn () => phasync\ext\include_global($file));
    $b->start();
    $b->resume();
    echo "again returned ", $b->getReturn(), "\n";
}
run();
?>
--EXPECT--
concurrent: refused
first returned 1, global 1
again returned 2
