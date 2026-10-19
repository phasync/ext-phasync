--TEST--
sapi(): request bodies far larger than any buffer reach php://input whole, in pieces and at once
--EXTENSIONS--
phasync
--INI--
upload_max_filesize=8M
post_max_size=16M
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$body = str_repeat(random_bytes(4096), 1024) . 'tail'; // 4 MiB + 4

$s = new Sink;
$s->body = $body;
$s->info = ['method' => 'PUT', 'content_type' => 'application/octet-stream', 'content_length' => strlen($body)];
$r = sapi(function () {
    $in = fopen('php://input', 'r');
    $h = hash_init('md5');
    $n = $reads = 0;
    while ('' !== ($c = fread($in, 65536))) {
        hash_update($h, $c);
        $n += strlen($c);
        $reads++;
    }
    return [$n, hash_final($h), feof($in), md5(file_get_contents('php://input'))];
}, $s);
var_dump($r[0] === strlen($body), $r[1] === md5($body), $r[2], $r[3] === md5($body));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
