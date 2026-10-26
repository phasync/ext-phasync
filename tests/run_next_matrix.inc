<?php
// run_next_matrix_*.phpt: every way a request can end, in every place it can end, with run_next() registered before
// (the next request is clean: run_next_probe.inc) and not registered (the process ends as stock PHP's would).
require __DIR__ . '/run_next.inc';

const RN_MODES = [
    'normal', 'exit', 'exit_int', 'exit_string', 'die', 'exception', 'type_error', 'parse_error', 'included_error',
    'redeclare', 'compile_error', 'memory', 'timeout', 'recursion', 'stack_overflow', 'user_error', 'error_to_exception',
    'exception_handler', 'destructor_exception', 'fiber_exception', 'fiber_suspended', 'fibers_many',
];

function rn_matrix(string $ctx): void
{
    $d = rn_dir();
    $o = ['load' => ['mbstring'], 'env' => getenv() + ['RN_BASE' => "$d/base"]];
    $probe = __DIR__ . '/run_next_probe.inc';
    [$out, $status] = rn_exec($d, $probe, [], null, ['env' => $o['env'] + ['RN_WRITE' => '1', 'RN_LOG' => "$d/none"]] + $o);
    if ($out !== "baseline written\n" || $status !== 0) {
        echo "baseline: $out (status $status)\n";
    }
    $fails = 0;
    // the depth reached before the stack check fires depends on the frames below it, so a stack overflow's trace is cut
    $norm = fn(string $s) => preg_replace(['/tried to allocate \d+ bytes/', '/^#\d+ .*\n/m'], ['tried to allocate N bytes', ''], $s);
    foreach (RN_MODES as $mode) {
        if ($mode === 'timeout' && !in_array($ctx, ['main', 'shutdown'])) {
            continue;
        }
        $run = function (string $variant, bool $ext) use ($d, $o, $mode, $ctx, $probe, $norm) {
            $env = $o['env'] + ['RN_LOG' => "$d/log_$variant"];
            @unlink("$d/log_$variant");
            [$out, $status] = rn_exec($d, __DIR__ . '/run_next_fail.inc', [$mode, $ctx, $variant === 'reg' ? 'reg' : 'noreg', "$d/log_$variant", $probe],
                null, ['ext' => $ext, 'env' => $env] + $o);
            return [$norm($out), $status];
        };
        [$noreg, $noregStatus] = $run('noreg', true);
        if ($ctx !== 'loop') {
            [$stock, $stockStatus] = $run('stock', false);
            if ([$noreg, $noregStatus] !== [$stock, $stockStatus]) {
                $fails++;
                echo "$mode in $ctx, not registered: differs from stock PHP\n--- stock (status $stockStatus)\n$stock--- ext-phasync (status $noregStatus)\n$noreg";
            }
        }
        [$reg, $regStatus] = $run('reg', true);
        if ($reg !== $noreg . "probe ok\n" || $regStatus !== 0) {
            $fails++;
            echo "$mode in $ctx, registered: next request is not clean (status $regStatus)\n--- expected\n{$noreg}probe ok\n--- got\n$reg";
        }
    }
    echo "failures: $fails\n";
    rn_clean($d);
}
