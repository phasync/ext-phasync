--TEST--
sapi(): header() with a newline or NUL in it warns and drops the header, as PHP's own SAPIs do (php-src bug60227_*.phpt; #11)
--EXTENSIONS--
phasync
--INI--
expose_php=0
default_charset=UTF-8
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

foreach (["X-foo: e\n foo", "X-Foo6: e\0Set-Cookie: ID=\n123\n d", "X-foo: e\r\nfoo"] as $h) {
    $s = new Sink;
    sapi(function () use ($h) { header($h); echo 'foo'; }, $s);
    echo trim($s->out), "\n", json_encode($s->headers[2]), "\n";
}
?>
--EXPECTF--
Warning: Header may not contain more than a single header, new line detected in %s on line %d
foo
["Content-type: text\/html; charset=UTF-8"]
Warning: Header may not contain NUL bytes in %s on line %d
foo
["Content-type: text\/html; charset=UTF-8"]
Warning: Header may not contain more than a single header, new line detected in %s on line %d
foo
["Content-type: text\/html; charset=UTF-8"]
