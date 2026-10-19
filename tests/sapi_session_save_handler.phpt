--TEST--
sapi(): session_set_save_handler() inside a request replaces the handler for that request only; the worker's own handler (set outside any sapi() call) keeps serving every other request
--EXTENSIONS--
phasync
session
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--INI--
session.use_strict_mode=0
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

class MemoryHandler implements SessionHandlerInterface {
    public array $data = [];
    public function open(string $p, string $n): bool { return true; }
    public function close(): bool { return true; }
    public function read(string $id): string|false { return $this->data[$id] ?? ''; }
    public function write(string $id, string $d): bool { $this->data[$id] = $d; return true; }
    public function destroy(string $id): bool { unset($this->data[$id]); return true; }
    public function gc(int $max): int|false { return 0; }
}

// The worker sets its own handler, outside any sapi() call.
$worker = new MemoryHandler;
session_set_save_handler($worker, false);

sapi(function () {
    session_id('wh-a');
    session_start();
    $_SESSION['who'] = 'a';
}, new Sink);

// A request that sets its own handler replaces it for itself only.
$own = new MemoryHandler;
sapi(function () use ($own) {
    session_set_save_handler($own, false);
    session_id('wh-own');
    session_start();
    $_SESSION['who'] = 'own';
}, new Sink);

// The worker's handler got the first request's session, not the second's.
var_dump($worker->data);
var_dump($own->data);

// The worker's own handler is unaffected and still works for the next request.
$seen = null;
sapi(function () use (&$seen) {
    session_id('wh-a');
    session_start();
    $seen = $_SESSION;
}, new Sink);
var_dump($seen);
?>
--EXPECT--
array(1) {
  ["wh-a"]=>
  string(12) "who|s:1:"a";"
}
array(1) {
  ["wh-own"]=>
  string(14) "who|s:3:"own";"
}
array(1) {
  ["who"]=>
  string(1) "a"
}
