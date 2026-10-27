<?php
// A stand-in for phasync's run(), go() and await(), to show what the rewrite produced.
final class phasync
{
    static function run(Closure $f, ...$a) { return 'run(' . $f(...$a) . ')'; }
    static function go(Closure $f, ...$a) { return 'go(' . $f(...$a) . ')'; }
    static function await($x) { return "await($x)"; }
}
