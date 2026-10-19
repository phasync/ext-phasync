--TEST--
sapi(): a large multipart upload is parsed by request_parse_body() from the request's own body; the file reaches the host whole
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (PHP_VERSION_ID < 80400) die('skip request_parse_body() needs PHP 8.4'); ?>
--INI--
upload_max_filesize=8M
post_max_size=16M
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$file = str_repeat(random_bytes(4096), 1536); // 6 MiB
$bd = 'Bnd' . bin2hex(random_bytes(4));
$s = new Sink;
$s->body = "--$bd\r\nContent-Disposition: form-data; name=\"k\"\r\n\r\nv\r\n"
    . "--$bd\r\nContent-Disposition: form-data; name=\"f\"; filename=\"big.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n$file\r\n--$bd--\r\n";
$s->info = ['method' => 'POST', 'content_type' => "multipart/form-data; boundary=$bd", 'content_length' => strlen($s->body)];
$r = sapi(function () { [$post, $files] = request_parse_body(); return [$post, $files['f']['error'], $files['f']['size'], md5_file($files['f']['tmp_name'])]; }, $s);
var_dump($r[0], $r[1], $r[2], $r[3] === md5($file));
?>
--EXPECT--
array(1) {
  ["k"]=>
  string(1) "v"
}
int(0)
int(6291456)
bool(true)
