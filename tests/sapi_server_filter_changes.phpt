--TEST--
sapi(): a value the input filter changes is registered as changed; as ext/filter does it (its copies) (ext-phasync#20)
--EXTENSIONS--
phasync
filter
--INI--
filter.default=full_special_chars
error_reporting=E_ALL & ~E_DEPRECATED
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
BIG copied
1048576 dotted &lt;b&gt;
1048576 <b>
x
