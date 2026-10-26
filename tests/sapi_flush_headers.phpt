--TEST--
sapi(): flush() with no output yet sends the headers first, as a real SAPI's own flush callback does (sapi_cgibin_flush()/sapi_cli_server_flush() in php-src) -- a status/headers set just before (http_response_code(101)/header(), for an upgrade) must reach $sapi->send_headers() before $sapi->flush() runs, not after (#14)
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--INI--
implicit_flush=0
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

// A $sapi that logs the ORDER of calls -- the bug left the final values
// correct (send_headers() still ran, eventually, with the right status),
// only the ORDER was wrong (flush() reached the SAPI before send_headers()
// did), so only a call-order log catches it.
function logging_sapi(array &$log): object {
	return new class($log) {
		public function __construct(public array &$l) {}
		public function name(): string { return "test"; }
		public function write(string $d): bool { $this->l[] = "write:$d"; return true; }
		public function send_headers(int $s, ?string $sl, array $h): void { $this->l[] = "send_headers:$s"; }
		public function flush(): void { $this->l[] = 'flush'; }
	};
}

$log = [];
sapi(function () {
	http_response_code(101);
	header('Upgrade: websocket');
	flush();
	echo 'body';
}, logging_sapi($log));
var_dump($log);

// A second flush(), headers already sent: no second send_headers() call.
$log2 = [];
sapi(function () {
	echo 'first ';
	flush();
	echo 'second';
	flush();
}, logging_sapi($log2));
var_dump($log2);
?>
--EXPECT--
array(3) {
  [0]=>
  string(16) "send_headers:101"
  [1]=>
  string(5) "flush"
  [2]=>
  string(10) "write:body"
}
array(5) {
  [0]=>
  string(16) "send_headers:200"
  [1]=>
  string(12) "write:first "
  [2]=>
  string(5) "flush"
  [3]=>
  string(12) "write:second"
  [4]=>
  string(5) "flush"
}
