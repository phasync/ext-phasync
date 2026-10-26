<?php
// A $sapi for phasync\ext\sapi() that records what a SAPI would receive.
class Sink
{
    public string $out = '';
    public ?array $headers = null;     // [status, statusLine, lines] from send_headers()
    public int $flushes = 0;
    public bool $gone = false;         // writes are recorded, then fail: the client is gone
    public string $body = '';          // what read() serves
    public array $info = [];           // returned by request_info()
    public ?Closure $onWrite = null;   // runs before each write records (may suspend)

    public function write(string $data): bool
    {
        if ($this->onWrite) ($this->onWrite)($data);
        $this->out .= $data;
        return !$this->gone;
    }

    public function read(int $length): string
    {
        $r = substr($this->body, 0, $length);
        $this->body = (string) substr($this->body, $length);
        return $r;
    }

    public function send_headers(int $status, ?string $statusLine, array $headers): void
    {
        $this->headers = [$status, $statusLine, $headers];
    }

    public function name(): string { return 'testsapi'; }

    public function flush(): void { $this->flushes++; }

    public function request_info(): array { return $this->info; }
}
