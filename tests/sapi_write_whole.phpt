--TEST--
sapi(): output in a coroutine reaches $sapi->write() whole, not cut into PIPE_BUF pieces by the CLI's stdout hook
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
require __DIR__ . '/loop.inc';
use function phasync\ext\sapi;

$s = new class extends Sink {
    public array $sizes = [];
    public function write(string $data): bool { $this->sizes[] = strlen($data); return parent::write($data); }
};
(new Loop)->runAll(function () use ($s) {
    sapi(function () { echo str_repeat('x', 131072); }, $s);
});
echo implode(',', $s->sizes), "\n";
?>
--EXPECT--
131072
