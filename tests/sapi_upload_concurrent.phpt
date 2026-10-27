--TEST--
sapi(): a multipart upload whose body arrives slowly (read() suspends) while another request runs in its own Scope: each request gets its own, complete $_POST and $_FILES (the body is read before PHP parses it) (#10)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

final class SlowSink extends Sink
{
    public function read(int $length): string
    {
        Fiber::suspend();                         // the client is slow: others run meanwhile
        return parent::read(min($length, 1000));
    }
}
function upload(string $name, string $data): SlowSink
{
    $bd = 'B' . $name;
    $s = new SlowSink;
    $s->body = "--$bd\r\nContent-Disposition: form-data; name=\"who\"\r\n\r\n$name\r\n"
        . "--$bd\r\nContent-Disposition: form-data; name=\"f\"; filename=\"$name.bin\"\r\n\r\n$data\r\n--$bd--\r\n";
    $s->info = ['method' => 'POST', 'content_type' => "multipart/form-data; boundary=$bd", 'content_length' => strlen($s->body)];
    return $s;
}
$data = ['a' => str_repeat('A', 5000), 'b' => str_repeat('B', 7000)];
$log = [];
$fibers = [];
foreach ($data as $name => $bytes) {
    $s = upload($name, $bytes);
    $code = function () use ($name, $bytes, &$log) {
        $log[] = "$name: who=" . $_POST['who'] . ' file=' . $_FILES['f']['name'] . ' '
            . (file_get_contents($_FILES['f']['tmp_name']) === $bytes ? 'intact' : 'CORRUPT') . ' files=' . count($_FILES);
    };
    $fibers[$name] = new Fiber(fn() => (new Scope)->run(fn() => sapi($code, $s)));
}
foreach ($fibers as $f) $f->start();
while (array_filter($fibers, fn($f) => !$f->isTerminated())) {
    foreach ($fibers as $f) if ($f->isSuspended()) $f->resume();
}
sort($log);
echo implode("\n", $log), "\n";
var_dump($_FILES, $_POST);
?>
--EXPECT--
a: who=a file=a.bin intact files=1
b: who=b file=b.bin intact files=1
array(0) {
}
array(0) {
}
