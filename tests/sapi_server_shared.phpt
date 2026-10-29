--TEST--
sapi(): $_SERVER shares the given strings instead of copying them per request (ext/filter under unsafe_raw too: $_SERVER and its filter_input() copy), keys mangled as PHP does (ext-phasync#20)
--EXTENSIONS--
phasync
filter
--INI--
filter.default=unsafe_raw
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$s = new Sink;
$s->info = ['server' => ['BIG' => str_repeat('x', 1 << 20), 'A.B' => 'dotted', 'HTML' => '<b>']];
$before = memory_get_usage();
sapi(function () use ($before) {
    // A copy of BIG would be 1 MiB more.
    echo memory_get_usage() - $before < 512 * 1024 ? "BIG shared\n" : "BIG copied\n";
    echo strlen($_SERVER['BIG']), ' ', $_SERVER['A_B'], ' ', $_SERVER['HTML'], "\n";
    echo strlen(filter_input(INPUT_SERVER, 'BIG')), ' ', filter_input(INPUT_SERVER, 'HTML', FILTER_UNSAFE_RAW), "\n";
    $_SERVER['BIG'][0] = 'y';          // the request's own copy-on-write: the given string is unchanged
}, $s);
echo $s->out;
echo $s->info['server']['BIG'][0], "\n";
?>
--EXPECT--
BIG shared
1048576 dotted <b>
1048576 <b>
x
