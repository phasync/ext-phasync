--TEST--
Preemption inside a Swoole coroutine: Swoole's nameless frame at the bottom of each coroutine is a C frame never suspended across, not a crash
--EXTENSIONS--
phasync
swoole
--FILE--
<?php
use phasync\ext\Suspension;

// without ext-curl, Swoole warns for each curl function it would hook
$flags = SWOOLE_HOOK_ALL;
foreach (['SWOOLE_HOOK_CURL', 'SWOOLE_HOOK_NATIVE_CURL'] as $c) {
    if (!extension_loaded('curl') && defined($c)) {
        $flags &= ~constant($c);
    }
}
Swoole\Coroutine::set(['hook_flags' => $flags]);

Suspension::setPreemptInterval(0.001);
Swoole\Coroutine\run(function () {
    // checkpoints in code a Swoole coroutine runs, with no fiber: the stack walk reaches Swoole's frame
    $t = microtime(true);
    while (microtime(true) - $t < 0.05);
    echo "a busy loop in a coroutine ran\n";

    // a managed fiber inside a coroutine is preempted as usual
    $f = Suspension::manage(new Fiber(function () {
        $t = microtime(true);
        while (microtime(true) - $t < 0.05);
        return 'the fiber ran';
    }));
    $resume = fn() => $f->start();
    $preempted = 0;
    for (;;) {
        try {
            $resume();
            break;
        } catch (Suspension $s) {
            $preempted++;
            $resume = fn() => $s->resume();
        }
    }
    echo $f->getReturn(), ', preempted: ', $preempted > 0 ? 'yes' : 'no', "\n";
});
Suspension::setPreemptInterval(null);
echo "done\n";
?>
--EXPECT--
a busy loop in a coroutine ran
the fiber ran, preempted: yes
done
