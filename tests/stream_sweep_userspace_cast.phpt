--TEST--
manage()'s stream sweep leaves a userspace wrapper's stream alone: its stream_cast() fd is its own to close (#87)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';
class W {
    public static $inner;
    public $context;
    function stream_open($p, $m, $o, &$op) { return true; }
    function stream_cast($as) { return self::$inner; }
    function stream_read($n) { return false; }
    function stream_eof() { return true; }
}
stream_wrapper_register('ww', 'W');
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
W::$inner = $a;
$u = fopen('ww://x', 'r');   // opened before manage(): its sweep must not wrap it

$loop = new Loop;
$loop->runAll(
    function () use ($loop) {
        // Parked on something else while the other coroutine closes $u: the
        // Poller's deregistration sweep (phasync_stream_forget()) then has a
        // reason to re-cast every closing stream, $u included.
        [$x, $y] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
        try {
            $loop->poller->readable($x, 0.2);
        } catch (LoopTimeout) {
        }
        fclose($x);
        fclose($y);
    },
    function () use ($u, $a) {
        fclose($a);
        var_dump(fclose($u));
    },
);
fclose($b);
echo "done\n";
?>
--EXPECT--
bool(true)
done
