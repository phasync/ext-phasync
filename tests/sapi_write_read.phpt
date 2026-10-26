--TEST--
sapi(): output goes to $sapi->write() piece by piece, outside the request (its own echo goes to the process's output); php://input reads $sapi->read() until it returns ''
--EXTENSIONS--
phasync
--FILE--
<?php
use function phasync\ext\sapi;

$s = new class {
    public array $writes = [];
    public array $reads = [];
    public string $body;
    public function name(): string { return 'test'; }
    public function send_headers(int $s, ?string $l, array $h): void {}
    public function write(string $data): bool
    {
        $this->writes[] = $data;
        echo "[write() sees PHP_SAPI ", PHP_SAPI, "]\n";
        return true;
    }
    public function read(int $length): string
    {
        $r = substr($this->body, 0, min($length, 5000));   // short reads: PHP asks again
        $this->body = (string) substr($this->body, strlen($r));
        $this->reads[] = strlen($r);
        return $r;
    }
    public function request_info(): array { return ['method' => 'POST', 'content_length' => 12000]; }
};
$s->body = str_repeat('x', 12000);
sapi(function () use (&$len) {
    echo 'a', 'b';
    $len = strlen(file_get_contents('php://input'));
}, $s);
var_dump($s->writes, $len, end($s->reads));
?>
--EXPECT--
[write() sees PHP_SAPI cli]
[write() sees PHP_SAPI cli]
array(2) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
}
int(12000)
int(0)
