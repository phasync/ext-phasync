--TEST--
sapi(): a userland session handler may suspend; two interleaved requests each keep their own session id, $_SESSION and write()
--EXTENSIONS--
phasync
session
--INI--
session.use_strict_mode=0
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
require __DIR__ . '/loop.inc';
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

class MemoryHandler implements SessionHandlerInterface {
    public array $data = [];
    public array $seen = [];
    private function log(string $call, string $id): void {
        $this->seen[] = "$call($id)" . (session_id() === $id ? '' : ' sees ' . session_id());
    }
    public function open(string $p, string $n): bool { return true; }
    public function close(): bool { $this->seen[] = 'close(' . session_id() . ')'; return true; }
    public function read(string $id): string|false {
        $this->log('read', $id);
        usleep($id === 'a' ? 50000 : 1000);   // A's read is still waiting while B's whole request runs
        $this->log('read done', $id);
        return $this->data[$id] ?? '';
    }
    public function write(string $id, string $d): bool { $this->log('write', $id); $this->data[$id] = $d; return true; }
    public function destroy(string $id): bool { return true; }
    public function gc(int $max): int|false { return 0; }
}

$handler = new MemoryHandler;
session_set_save_handler($handler, false);
$req = function (string $id) use (&$out) {
    return function () use ($id, &$out) {
        sapi(function () use ($id, &$out) {
            session_id($id);
            session_start();
            $_SESSION['who'] = $id;
            session_write_close();
            $out[$id] = $_SESSION;
        }, new Sink);
    };
};
(new Loop)->runAll($req('a'), $req('b'));
echo implode("\n", $handler->seen), "\n";
var_dump($out['a'], $out['b'], $handler->data);

$loop = new Loop;
$loop->runAll(function () use (&$back) {
    sapi(function () use (&$back) { session_id('a'); session_start(); $back = $_SESSION; }, new Sink);
});
var_dump($back);
?>
--EXPECT--
read(a)
read(b)
read done(b)
write(b)
close(b)
read done(a)
write(a)
close(a)
array(1) {
  ["who"]=>
  string(1) "a"
}
array(1) {
  ["who"]=>
  string(1) "b"
}
array(2) {
  ["b"]=>
  string(12) "who|s:1:"b";"
  ["a"]=>
  string(12) "who|s:1:"a";"
}
array(1) {
  ["who"]=>
  string(1) "a"
}
