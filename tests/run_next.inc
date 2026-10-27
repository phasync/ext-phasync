<?php
// Helpers for run_next tests: scripts are written to a temp dir and run by a child php.
function rn_dir(): string
{
    $d = sys_get_temp_dir() . '/rn_' . getmypid() . '_' . bin2hex(random_bytes(3));
    mkdir($d);
    return $d;
}

/**
 * Runs $script in a child php; returns [stdout+stderr with $dir replaced by DIR, exit status].
 * $o: ext (default true: load ext-phasync), load (system extensions), ini (name => value), env (name => value),
 * stdin (a path to feed it, default /dev/null), keep ($dir not replaced in the output).
 */
function rn_exec(string $dir, string $script, array $args = [], ?string $cwd = null, array $o = []): array
{
    $cmd = [PHP_BINARY, '-n'];
    if ($o['ext'] ?? true) {
        $cmd[] = '-d';
        $cmd[] = 'extension=' . realpath(ini_get('extension_dir') . '/phasync.so');
    }
    // The children fork and use alarms. An extension is loaded where it is shared; where it is
    // compiled in (curl and pcntl differ between builds) there is no file to load
    foreach (['pcntl', ...$o['load'] ?? []] as $ext) {
        if (is_file($so = PHP_EXTENSION_DIR . "/$ext.so")) {
            $cmd[] = '-d';
            $cmd[] = "extension=$so";
        }
    }
    foreach (['error_reporting' => '-1', 'log_errors' => '0'] + ($o['ini'] ?? []) as $k => $v) {
        $cmd[] = '-d';
        $cmd[] = "$k=$v";
    }
    $cmd[] = $script;
    array_push($cmd, ...$args);
    $p = proc_open($cmd, [0 => ['file', $o['stdin'] ?? '/dev/null', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, $cwd ?? $dir, $o['env'] ?? null);
    $out = stream_get_contents($pipes[1]);
    $status = proc_close($p);
    return [($o['keep'] ?? false) ? $out : str_replace($dir, 'DIR', $out), $status];
}

function rn_clean(string $dir): void
{
    foreach (glob("$dir/*") as $f) {
        is_dir($f) ? rn_clean($f) : unlink($f);
    }
    rmdir($dir);
}
