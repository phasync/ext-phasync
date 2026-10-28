--TEST--
sapi(): zlib.output_compression compresses the request's output when it accepts gzip (as RINIT does under FPM)
--EXTENSIONS--
phasync
zlib
--INI--
zlib.output_compression=1
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

foreach (['gzip', 'identity'] as $enc) {
    $s = new Sink;
    $s->info = ['server' => ['HTTP_ACCEPT_ENCODING' => $enc]];
    sapi(function () { header('Vary: Cookie'); echo 'foo'; }, $s);
    $h = array_values(array_filter(array_map('strtolower', $s->headers[2]), fn($l) => preg_match('/^(vary|content-encoding):/', $l)));
    sort($h);
    echo $enc, ': ', implode(' | ', $h), ' / ', $enc === 'gzip' ? gzdecode($s->out) : $s->out, "\n";
}
?>
--EXPECT--
gzip: content-encoding: gzip | vary: accept-encoding | vary: cookie / foo
identity: vary: cookie / foo
