<?php
// A $sapi for phasync\ext\sapi() that records what a SAPI would receive.
class Sink
{
    public string $out = '';
    public ?array $headers = null;     // [status, statusLine, lines] from send_headers()
    public int $flushes = 0;
    public bool $gone = false;         // writes to output() are recorded, then fail: the client is gone
    public string $body = '';          // read from input()
    public array $info = [];           // returned by request_info()
    public ?Closure $onWrite = null;   // runs before each write to output() records (may suspend)

    /** @return resource */
    public function output()
    {
        return SinkStream::open($this, 'w');
    }

    /** @return resource */
    public function input()
    {
        return SinkStream::open($this, 'r');
    }

    public function send_headers(int $status, ?string $statusLine, array $headers): void
    {
        $this->headers = [$status, $statusLine, $headers];
    }

    public function name(): string { return 'testsapi'; }

    public function flush(): void { $this->flushes++; }

    public function request_info(): array { return $this->info; }
}

// sink:// streams: writes append to a Sink's $out, reads take its $body;
// with a Closure, each write is passed to it.
class SinkStream
{
    public static array $next = [];
    public $context;
    private Sink|Closure $sink;

    public static function open(Sink|Closure $sink, string $mode)
    {
        self::$next[] = $sink;
        return fopen('sink://', $mode);
    }

    public function stream_open(string $path, string $mode, int $options, ?string &$opened): bool
    {
        $this->sink = array_pop(self::$next);
        return true;
    }

    public function stream_write(string $data): int
    {
        if ($this->sink instanceof Closure) {
            ($this->sink)($data);
            return strlen($data);
        }
        if ($this->sink->onWrite) ($this->sink->onWrite)($data);
        $this->sink->out .= $data;
        return $this->sink->gone ? 0 : strlen($data);
    }

    public function stream_read(int $length): string
    {
        $r = substr($this->sink->body, 0, $length);
        $this->sink->body = (string) substr($this->sink->body, $length);
        return $r;
    }

    public function stream_eof(): bool { return $this->sink->body === ''; }
}
stream_wrapper_register('sink', SinkStream::class);
