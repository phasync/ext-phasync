--TEST--
sapi(): session upload progress is recorded while the request's multipart body is read (as under FPM)
--EXTENSIONS--
phasync
session
--INI--
file_uploads=1
session.use_strict_mode=0
session.use_cookies=1
session.use_only_cookies=1
session.cache_limiter=
session.upload_progress.enabled=1
session.upload_progress.cleanup=0
session.upload_progress.prefix=upload_progress_
session.upload_progress.name=PHP_SESSION_UPLOAD_PROGRESS
session.upload_progress.freq=1%
session.upload_progress.min_freq=0.000000001
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

$dir = sys_get_temp_dir() . '/phasync_sapi_upprog_' . getmypid();
@mkdir($dir);
ini_set('session.save_path', $dir);

$bd = 'XyZ';
$s = new Sink;
$s->body = "--$bd\r\nContent-Disposition: form-data; name=\"PHP_SESSION_UPLOAD_PROGRESS\"\r\n\r\nkey1\r\n"
    . "--$bd\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\nContent-Type: text/plain\r\n\r\nfile\r\n--$bd--\r\n";
$s->info = ['method' => 'POST', 'content_type' => "multipart/form-data; boundary=$bd",
    'content_length' => strlen($s->body), 'cookie' => 'PHPSESSID=upprog-id'];
$r = null;
sapi(function () use (&$r) {
    $ok = session_start();
    $p = $_SESSION['upload_progress_key1'] ?? null;
    $r = [$ok, session_id(), $p['content_length'] ?? null, $p['done'] ?? null, $p['files'][0]['name'] ?? null, $p['files'][0]['done'] ?? null];
    session_destroy();
}, $s);
var_dump(...$r);
array_map("unlink", glob("$dir/sess_*"));
rmdir($dir);
?>
--EXPECTF--
bool(true)
string(9) "upprog-id"
int(%d)
bool(true)
string(5) "a.txt"
bool(true)
