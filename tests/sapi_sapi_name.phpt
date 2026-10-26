--TEST--
sapi(): PHP_SAPI and php_sapi_name() are the name the $sapi object gives with name() inside a request and "cli" outside (issue #57)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

function name_of() { return PHP_SAPI . ' ' . php_sapi_name() . ' ' . constant('PHP_SAPI'); }
echo name_of(), "\n";
$s = new class extends Sink { public function name(): string { return 'hostsapi'; } };
$x = new Fiber(fn () => sapi(function () {
    echo name_of(), "\n";
    Fiber::suspend();
    echo name_of(), "\n";
}, $s));
$x->start();
echo name_of(), "\n";
$x->resume();
echo $s->out;
echo name_of(), "\n";

try {
    sapi(function () {}, new class { public function name(): int { return 1; } public function write(string $d): bool { return true; } public function send_headers(int $s, ?string $l, array $h): void {} });
} catch (Throwable $e) { echo get_class($e), "\n"; }
?>
--EXPECT--
cli cli cli
cli cli cli
hostsapi hostsapi hostsapi
hostsapi hostsapi hostsapi
cli cli cli
TypeError
