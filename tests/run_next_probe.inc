<?php
// The residue probe of run_next_matrix.phpt: the first request of a process records what a pristine request looks like,
// a later one prints "probe ok" or what differs.
// env: RN_BASE (the baseline file), RN_WRITE (record it), RN_LOG (what the failing request logged)
declare(ticks=1);
require __DIR__ . '/loop.inc';

function rn_snapshot(): array
{
    $classes = array_merge(get_declared_classes(), get_declared_interfaces(), get_declared_traits());
    sort($classes);
    $fds = [];
    $std = array_map(fn($fd) => readlink("/proc/self/fd/$fd"), [0, 1, 2]);
    foreach (scandir('/proc/self/fd') as $fd) {
        // from the second request on, STDIN, STDOUT and STDERR are dups of descriptors 0-2 (what php://stdin does after its first use)
        if ($fd[0] !== '.' && ($fd < 3 || !in_array($t = @readlink("/proc/self/fd/$fd"), $std, true))) {
            $fds[] = preg_replace('/\[\d+\]/', '', @readlink("/proc/self/fd/$fd"));
        }
    }
    // the extension keeps one descriptor on the mount table for the life of the process, once a file system call needed it
    $fds = array_filter($fds, fn($t) => !str_ends_with($t, '/mountinfo'));
    sort($fds);
    $res = array_values(array_diff_key(get_resources(), [(int) STDIN => 1, (int) STDOUT => 1, (int) STDERR => 1]));
    $res = array_map('get_resource_type', $res);
    sort($res);
    $server = $_SERVER;
    $server = array_filter($server, fn($k) => !str_starts_with($k, 'RN_'), ARRAY_FILTER_USE_KEY);
    unset($server['REQUEST_TIME'], $server['REQUEST_TIME_FLOAT'], $server['argv'], $server['argc'], $server['PHP_SELF'], $server['SCRIPT_NAME'], $server['SCRIPT_FILENAME'], $server['PATH_TRANSLATED']);
    $env = array_filter(getenv(), fn($k) => !str_starts_with($k, 'RN_'), ARRAY_FILTER_USE_KEY);
    ksort($env);
    $a = mt_rand();
    mt_srand(42);
    $b = mt_rand();
    $eh = set_error_handler(fn() => false);
    restore_error_handler();
    $xh = set_exception_handler(fn() => false);
    restore_exception_handler();
    return [
        'user functions' => get_defined_functions()['user'],
        'declared classes, interfaces and traits' => $classes,
        'user constants' => get_defined_constants(true)['user'] ?? [],
        'included files' => get_included_files(),
        'global variable names' => array_keys($GLOBALS),   // argv and argc: the first request's
        'ob level' => ob_get_level(),
        'ob length' => ob_get_length(),
        'error handler' => $eh,
        'exception handler' => $xh,
        'autoloaders' => spl_autoload_functions(),
        'ini' => ini_get_all(null, false),
        'error_get_last' => error_get_last(),
        'gc_status' => array_intersect_key(gc_status(), ['runs' => 1, 'collected' => 1, 'threshold' => 1]),
        'current fiber' => Fiber::getCurrent(),
        'resources' => $res,
        'open descriptors' => $fds,
        '$_SERVER' => $server,
        '$_ENV' => array_filter($_ENV, fn($k) => !str_starts_with($k, 'RN_'), ARRAY_FILTER_USE_KEY),
        '$_GET' => $_GET,
        '$_POST' => $_POST,
        '$_COOKIE' => $_COOKIE,
        '$_FILES' => $_FILES,
        '$_REQUEST' => $_REQUEST,
        'environment' => $env,
        'mt_rand is not the sequence of the last seed' => $a !== $b,
        'locale' => setlocale(LC_ALL, 0),
        'umask' => umask(),
        'cwd' => getcwd(),
        'timezone' => date_default_timezone_get(),
        'mb_internal_encoding' => function_exists('mb_internal_encoding') ? mb_internal_encoding() : null,
        'sapi' => PHP_SAPI,
    ];
}

function rn_diff($a, $b): string
{
    if (is_array($a) && is_array($b)) {
        $keys = array_filter(array_unique([...array_keys($a), ...array_keys($b)]), fn($k) => ($a[$k] ?? null) !== ($b[$k] ?? null));
        return 'differing keys ' . substr(json_encode(array_map(fn($k) => [$k, $a[$k] ?? null, $b[$k] ?? null], array_values($keys)), JSON_PARTIAL_OUTPUT_ON_ERROR), 0, 600);
    }
    return 'expected ' . substr(json_encode($a, JSON_PARTIAL_OUTPUT_ON_ERROR), 0, 300) . ', got ' . substr(json_encode($b, JSON_PARTIAL_OUTPUT_ON_ERROR), 0, 300);
}

function rn_loop(): string
{
    $loop = new Loop;
    $out = [];
    $loop->runAll(
        function () use ($loop, &$out) { $loop->sleep(20000); $out[] = 'slept'; },
        function () use ($loop, &$out) {
            [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
            $loop->go(function () use ($b) { fwrite($b, "ping"); });
            $loop->poller->readable($a);
            $out[] = fread($a, 4);
        },
        function () use ($loop, &$out) { $loop->sleep(1000); $out[] = 'first'; },
    );
    return implode(',', $out);
}

$file = getenv('RN_BASE');
$log = getenv('RN_LOG');
$before = rn_snapshot();
$memory = [memory_get_usage(), memory_get_peak_usage()];
$loopResult = rn_loop();
declare(ticks=1) { $t = 1; $t++; }
$lines = is_file($log) ? array_count_values(file($log, FILE_IGNORE_NEW_LINES)) : [];
gc_collect_cycles();
$after = rn_snapshot();
if (getenv('RN_WRITE')) {
    file_put_contents($file, serialize([$before, $memory]));
    echo "baseline written\n";
    return;
}
[$base, $baseMemory] = unserialize(file_get_contents($file));
$bad = [];
foreach ($base as $k => $v) {
    if ($before[$k] !== $v) {
        $bad[] = "$k: " . rn_diff($v, $before[$k]);
    }
}
if (($l = explode(',', $loopResult)) && sort($l) && $l !== ['first', 'ping', 'slept']) {
    $bad[] = "loop result: $loopResult";
}
if (isset($lines['tick'])) {
    $bad[] = 'a tick function of the previous request ran';
}
if (($lines['shutdown'] ?? 0) !== 1) {
    $bad[] = 'shutdown functions that ran: ' . json_encode($lines);
}
if (abs($memory[0] - $baseMemory[0]) > 262144 || abs($memory[1] - $baseMemory[1]) > 262144) {
    $bad[] = 'memory: ' . json_encode($memory) . ' vs ' . json_encode($baseMemory);
}
if ($after['open descriptors'] !== $base['open descriptors']) {
    $bad[] = 'descriptors after a loop: ' . json_encode($after['open descriptors']);
}
echo $bad ? "probe differs:\n  " . implode("\n  ", $bad) . "\n" : "probe ok\n";
