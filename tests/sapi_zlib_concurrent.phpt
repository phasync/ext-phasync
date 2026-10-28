--TEST--
sapi(): zlib state is per request (each in its own Scope, as hosts run them); interleaved requests (compression, plain, ob_gzhandler) each get their own encoding
--EXTENSIONS--
phasync
zlib
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--INI--
zlib.output_compression=1
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;
ob_start();

function req(string $enc, ?string $handler, string $tag, Sink $s): Fiber {
    $s->info = ['server' => ['HTTP_ACCEPT_ENCODING' => $enc]];
    return new Fiber(fn() => (new Scope)->run(fn() => sapi(function () use ($handler, $tag) {
        if ($handler) ob_start($handler);
        echo "$tag-1 ";
        Fiber::suspend();
        echo "$tag-2 ", str_repeat($tag, 50);
    }, $s)));
}
function run(array $specs) {
    $sinks = $f = [];
    foreach ($specs as $sp) { $sinks[] = $s = new Sink; $f[] = req($sp[0], $sp[1], $sp[2], $s); }
    foreach ($f as $x) $x->start();
    foreach (array_reverse($f) as $x) $x->resume();
    foreach ($sinks as $s) {
        $enc = array_values(array_filter($s->headers[2], fn($l) => stripos($l, 'content-encoding') === 0));
        $body = $enc ? gzdecode($s->out) : $s->out;
        echo json_encode($enc), ' ', substr($body, 0, 12), "\n";
    }
}
run([['gzip', null, 'a'], ['identity', null, 'b'], ['gzip', null, 'e']]);
ini_set('zlib.output_compression', '0');
run([['gzip', 'ob_gzhandler', 'c'], ['identity', 'ob_gzhandler', 'd'], ['gzip', null, 'f'], ['gzip', 'ob_gzhandler', 'g']]);
?>
--EXPECT--
["Content-Encoding: gzip"] a-1 a-2 aaaa
[] b-1 b-2 bbbb
["Content-Encoding: gzip"] e-1 e-2 eeee
["Content-Encoding: gzip"] c-1 c-2 cccc
[] d-1 d-2 dddd
[] f-1 f-2 ffff
["Content-Encoding: gzip"] g-1 g-2 gggg
