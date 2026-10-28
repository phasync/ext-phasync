<?php

declare(strict_types=1);

require __DIR__ . '/SwooleFibers.php';

use Swoole\Coroutine as Co;

$failures = 0;
function check(string $what, bool $ok, string $detail = ''): void
{
    global $failures;
    $failures += $ok ? 0 : 1;
    echo ($ok ? 'ok   ' : 'FAIL '), $what, '' === $detail ? '' : " ($detail)", "\n";
}

// Native fibers before any driver: untouched
$nf = new Fiber(fn($x) => Fiber::suspend($x * 2) + 1);
$v = $nf->start(5);
$nf->resume(10);
check('native Fiber unaffected (no driver)', 10 === $v && 11 === $nf->getReturn());

Co::set(['hook_flags' => SWOOLE_HOOK_ALL & ~SWOOLE_HOOK_CURL & ~(defined('SWOOLE_HOOK_NATIVE_CURL') ? SWOOLE_HOOK_NATIVE_CURL : 0)]);
$d = new SwooleFibers();

Co\run(function () use ($d) {
    // 1. suspend/resume round trip with Fiber's own API
    $f = $d->create(function (string $a) {
        $self = Fiber::getCurrent();
        $got = Fiber::suspend("first:$a");
        return [$got, $self];
    });
    check('pseudo_fiber() is a \Fiber', $f instanceof Fiber && Fiber::class === get_class($f));
    check('isStarted() false before start', !$f->isStarted() && !$f->isTerminated());
    $first = $f->start('x');
    check('start() returns the first Fiber::suspend() value', 'first:x' === $first, var_export($first, true));
    check('isSuspended() while parked', $f->isSuspended() && $f->isStarted() && !$f->isRunning());
    $r = $f->resume('second');
    check('resume() returns null when the task ends', null === $r);
    [$got, $self] = $f->getReturn();
    check('resume() value arrives, getReturn() works', 'second' === $got && $f->isTerminated());
    check('Fiber::getCurrent() in a task is its handle', $self === $f);
    check('Fiber::getCurrent() outside tasks is null', null === Fiber::getCurrent());

    // 2. hooked blocking calls run concurrently, interleaved with parks (the case that segfaulted)
    $t = microtime(true);
    $tasks = [];
    for ($i = 0; $i < 50; $i++) {
        $tasks[$i] = $d->create(function (int $i) {
            usleep(20_000 + ($i % 5) * 10_000);
            $x = Fiber::suspend($i);           // parked until the loop below resumes it
            usleep(10_000);
            file_get_contents(__FILE__);
            return $i + $x;
        });
        $tasks[$i]->start($i);                 // runs until usleep, returns null
    }
    $parked = 0;
    while ($parked < 50) {
        Co::sleep(0.005);
        foreach ($tasks as $i => $task) {
            if ($task->isSuspended()) {
                $task->resume(1000);
                $parked++;
            }
        }
    }
    while (array_filter($tasks, fn($task) => !$task->isTerminated())) {
        Co::sleep(0.005);
    }
    $sum = array_sum(array_map(fn($task) => $task->getReturn(), $tasks));
    $took = microtime(true) - $t;
    check('50 tasks interleave hooked sleeps and parks, no crash', 50 * 1000 + 1225 === $sum, "sum $sum");
    check('their blocking calls run concurrently', $took < 0.25, sprintf('%.2fs; serial would be 3.5s', $took));

    // 3. throw() into a parked task
    $f = $d->create(function () {
        try {
            Fiber::suspend();
        } catch (DomainException $e) {
            return 'caught ' . $e->getMessage();
        }
    });
    $f->start();
    $f->throw(new DomainException('boom'));
    check('throw() into a parked task', 'caught boom' === $f->getReturn(), $f->getReturn());

    // 4. throw() into a task blocked in a hooked call: the task gets $e, Swoole's exception as previous
    $f = $d->create(function () {
        try {
            usleep(2_000_000);
            return 'slept';
        } catch (Throwable $e) {
            return [get_class($e), $e->getMessage(), get_class($e->getPrevious() ?? new stdClass())];
        }
    });
    $f->start();
    $t = microtime(true);
    $f->throw(new DomainException('cancel'));
    while (!$f->isTerminated()) {
        Co::sleep(0.001);
    }
    $r = $f->getReturn();
    check('throw() into a hooked call delivers $e, quickly', [DomainException::class, 'cancel', Swoole\Coroutine\CanceledException::class] === $r && microtime(true) - $t < 0.1, json_encode($r));

    // 4b. a wait Swoole cannot interrupt (FIFO open, in its thread pool): $e is thrown when it ends
    $fifo = '/tmp/claude-1000/pf-test.fifo';
    @unlink($fifo);
    exec('mkfifo ' . escapeshellarg($fifo));
    $f = $d->create(function () use ($fifo) {
        try {
            $h = fopen($fifo, 'r');             // blocks in Swoole's thread pool
            return 'not thrown';
        } catch (DomainException $e) {
            return 'thrown: ' . $e->getMessage();
        }
    });
    $f->start();
    Co::sleep(0.02);
    $f->throw(new DomainException('late'));
    go(fn() => fclose(fopen($fifo, 'w')));      // let the open complete
    $t = microtime(true);
    while (!$f->isTerminated() && microtime(true) - $t < 2) {
        Co::sleep(0.005);
    }
    check('a wait Swoole can\'t interrupt throws $e when it ends', $f->isTerminated() && 'thrown: late' === $f->getReturn(), $f->isTerminated() ? $f->getReturn() : 'still running');
    @unlink($fifo);

    // 4c. structured concurrency in miniature: one child fails, its sibling blocked in a socket read is
    // cancelled with the parent's exception, the parent sees the first error
    $parent = $d->create(function () use ($d) {
        [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
        $self = Fiber::getCurrent();
        $children = [
            $d->create(function () use ($a) {
                try {
                    fread($a, 10);              // nobody writes: blocks until cancelled
                    return 'read';
                } catch (RuntimeException $e) {
                    return 'sibling cancelled: ' . $e->getMessage();
                }
            }),
            $d->create(function () use ($self) {
                usleep(20_000);
                $e = new LogicException('child failed');
                if ($self->isSuspended()) {
                    $self->throw($e);           // report to the parent
                }
            }),
        ];
        foreach ($children as $c) {
            $c->start();
        }
        try {
            Fiber::suspend();                   // wait for the children
        } catch (LogicException $first) {
            foreach ($children as $c) {
                if (!$c->isTerminated()) {
                    $c->throw(new RuntimeException('frame failed: ' . $first->getMessage()));
                }
            }
            while (!$children[0]->isTerminated()) {
                usleep(1000);
            }
            return [$first->getMessage(), $children[0]->getReturn()];
        }
    });
    $parent->start();
    while (!$parent->isTerminated()) {
        Co::sleep(0.005);
    }
    $r = $parent->getReturn();
    check('a failing child cancels its sibling\'s hooked read with our exception', ['child failed', 'sibling cancelled: frame failed: child failed'] === $r, json_encode($r));

    // 5. exceptions: escape from start() when synchronous; getReturn() FiberError either way
    $f = $d->create(fn() => throw new LogicException('sync'));
    try {
        $f->start();
        $msg = 'nothing';
    } catch (LogicException $e) {
        $msg = $e->getMessage();
    }
    check('an exception escapes start() like a Fiber\'s', 'sync' === $msg);
    $f = $d->create(function () { usleep(1000); throw new LogicException('async'); });
    $f->start();
    Co::sleep(0.01);
    try {
        $f->getReturn();
        $msg = 'nothing';
    } catch (FiberError $e) {
        $msg = $e->getMessage();
    }
    check('getReturn() after an async exception: FiberError', str_contains($msg, 'threw an exception'), $msg);

    // 6. FiberErrors as for Fibers
    $f = $d->create(fn() => usleep(1000));
    $f->start();
    try { $f->resume(); $msg = 'nothing'; } catch (FiberError $e) { $msg = $e->getMessage(); }
    check('resume() of a task that is not parked: FiberError', str_contains($msg, 'not suspended'), $msg);
    try { Fiber::suspend(); $msg = 'nothing'; } catch (FiberError $e) { $msg = $e->getMessage(); }
    check('Fiber::suspend() outside a task: FiberError', str_contains($msg, 'outside of fiber'), $msg);

    // 7. a native Fiber inside a plain coroutine, with a driver installed
    $nf = new Fiber(fn() => Fiber::suspend('native') . Fiber::getCurrent()::class);
    $v = $nf->start();
    $nf->resume('!');
    check('native Fiber unaffected (driver installed)', 'native' === $v && '!Fiber' === $nf->getReturn());

    // 8. handles are released
    Co::sleep(0.01);
    unset($f, $tasks, $task, $self, $parent);
    gc_collect_cycles();
    check('finished tasks\' handles are freed', 0 === $d->count(), $d->count() . ' left');
});

exit($failures);
