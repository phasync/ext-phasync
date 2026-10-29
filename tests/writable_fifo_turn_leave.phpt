--TEST--
fwrite(): a turn holder that is cancelled or destroyed passes the write turn on, the rest stay whole
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/writable_fifo.inc';

function line(string $c): string { return str_repeat($c, 200000) . "\n"; }

// A holds the turn (the socket is full), B and C queue behind it; A goes, B and C follow whole.
foreach (['cancel', 'destroy'] as $how) {
    [$a, $b, $filled] = full_pair();
    $loop = new Loop;
    $log = [];
    $mk = function (string $c) use ($a, &$log) {
        return function () use ($a, &$log, $c) {
            try {
                $log[] = $c . ' wrote ' . fwrite($a, line($c));
            } catch (Throwable $e) {
                $log[] = $c . ' ' . get_class($e);
            }
        };
    };
    $got = '';
    $reader = function () use ($b, $filled, &$got, &$log) {
        drain($b, $filled);
        while (strlen($got) < 400002) {
            phasync\ext\Suspension::readable($b);
            $got .= fread($b, 65536);
        }
    };
    if ($how === 'cancel') {
        $fa = $loop->start($mk('A'));
        $loop->start($mk('B'));
        $loop->start($mk('C'));
        $loop->cancel($fa);
        $loop->go($reader);
        $loop->run();
    } else {
        destroy_while_waiting($loop, 'refcount', fn() => fwrite($a, line('A')), after: [$reader], alongside: [$mk('B'), $mk('C')]);
    }
    echo "$how: ", implode(', ', $log), '; stream: ', var_export($got === line('B') . line('C'), true), "\n";
}
?>
--EXPECT--
cancel: A LoopCancelled, B wrote 200001, C wrote 200001; stream: true
  finally ran
destroy: B wrote 200001, C wrote 200001; stream: true
