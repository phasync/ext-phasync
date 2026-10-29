<?php
require_once __DIR__ . '/destroy.inc';

// A blocking pair whose $a end is full (writable() parks), and how much sits in it.
function full_pair(): array
{
    [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
    stream_set_blocking($a, false);
    $filled = 0;
    while ($n = @fwrite($a, str_repeat('x', 65536))) { $filled += $n; }
    stream_set_blocking($a, true);
    return [$a, $b, $filled];
}

function drain($b, int $filled): void
{
    $got = 0;
    while ($got < $filled) { $got += strlen(fread($b, 65536)); }
}

// A coroutine waiting for $a to be writable, logging how it ended.
function waiter(Loop $loop, array &$log, $a, string $name, ?float $timeout = null): Closure
{
    return function () use (&$log, $a, $name, $timeout) {
        try {
            $r = phasync\ext\Suspension::writable($a, $timeout);
            $log[] = $name . ($r ? ' ready' : ' timeout');
        } catch (Throwable $e) {
            $log[] = $name . ' ' . get_class($e);
        }
    };
}
