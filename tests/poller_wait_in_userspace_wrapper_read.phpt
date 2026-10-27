--TEST--
A Poller wait inside a user-space stream wrapper's stream_read() suspends the coroutine; others run meanwhile (#107)
--EXTENSIONS--
phasync
--SKIPIF--
<?php
if (!class_exists('Fiber')) die('skip requires Fibers');
?>
--FILE--
<?php
require __DIR__ . '/loop.inc';

// Origin: swerve-symfony WebSockets read through a stream wrapper whose
// stream_read() waits for the socket. The wrapper method is called by fread().
final class W
{
    public $context;
    public static Loop $loop;
    private $r;
    public function stream_open($p, $m, $o, &$op) { $this->r = stream_context_get_options($this->context)['wrp']['r']; return true; }
    public function stream_read($n)
    {
        self::$loop->poller->readable($this->r);
        return (string) fread($this->r, $n);
    }
    public function stream_eof() { return false; }
    public function stream_stat() { return []; }
}
stream_wrapper_register('wrp', W::class);
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP);
stream_set_blocking($a, false);

$loop = W::$loop = new Loop;
$log = [];
$loop->runAll(
    function () use ($a, &$log) {
        $f = fopen('wrp://', 'rb', false, stream_context_create(['wrp' => ['r' => $a]]));
        $log[] = 'read: ' . fread($f, 100);
    },
    function () use ($loop, $b, &$log) {
        usleep(100000);
        $log[] = 'writing';
        fwrite($b, 'hello');
    }
);
echo implode("\n", $log), "\n";
?>
--EXPECT--
writing
read: hello
