--TEST--
sapi(): phpinfo() and phpcredits() print HTML inside the request (as under FPM), text again after it
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

function kind() { ob_start(); phpcredits(CREDITS_GROUP); $c = ob_get_clean(); return str_contains($c, '<table') ? 'html' : 'text'; }
echo kind(), "\n";
sapi(function () {
    echo kind(), "\n";
    ob_start(); phpinfo(INFO_GENERAL); $i = ob_get_clean();
    echo str_contains($i, '<table') ? 'html' : 'text', "\n";
}, new class extends Sink { public function write(string $d): bool { echo $d; return true; } });
echo kind(), "\n";
?>
--EXPECT--
text
html
html
text
