<?php
// Helpers for run_next tests: scripts are written to a temp dir and run by a child php with ext-phasync.
function rn_dir(): string
{
    $d = sys_get_temp_dir() . '/rn_' . getmypid() . '_' . bin2hex(random_bytes(3));
    mkdir($d);
    return $d;
}

/** Runs $script in a child php; returns [stdout+stderr with $dir replaced by DIR, exit status]. */
function rn_exec(string $dir, string $script, array $args = [], ?string $cwd = null): array
{
    $so = realpath(ini_get('extension_dir') . '/phasync.so');
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension=' . escapeshellarg($so) . ' -d error_reporting=-1 '
        . escapeshellarg($script) . ' ' . implode(' ', array_map('escapeshellarg', $args)) . ' 2>&1';
    $p = proc_open($cmd, [1 => ['pipe', 'w']], $pipes, $cwd ?? $dir);
    $out = stream_get_contents($pipes[1]);
    $status = proc_close($p);
    return [str_replace($dir, 'DIR', $out), $status];
}

function rn_clean(string $dir): void
{
    foreach (glob("$dir/*") as $f) {
        unlink($f);
    }
    rmdir($dir);
}
