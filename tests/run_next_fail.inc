<?php
// A request of run_next_matrix.phpt that leaves state behind and ends in one of the ways a request can end.
// argv: <mode> <context> <reg|noreg> <log file> <probe script>
[, $mode, $ctx, $reg, $log, $probe] = $argv;
$dir = dirname($log);
if ($reg === 'reg') {
    phasync\ext\run_next($probe);
}

function already_declared() {}
function stale_static() { static $n = 0; return ++$n; }
class Stale { public static int $count = 0; const X = 1; }
class Thrower { function __destruct() { throw new Exception('from a destructor'); } }
class Holder { public static array $held = []; }
class Dtor { function __construct(private string $mode) {} function __destruct() { act($this->mode); } }
enum StaleEnum { case A; }
interface StaleI {}
trait StaleT {}

function mark(string $log): void
{
    define('STALE_CONST', 1);
    $GLOBALS['stale_global'] = 1;
    stale_static();
    Stale::$count++;
    ini_set('precision', '7');
    set_error_handler(fn() => false);
    spl_autoload_register(function ($c) {});
    register_tick_function(function () use ($log) { file_put_contents($log, "tick\n", FILE_APPEND); });
    putenv('STALE_ENV=1');
    $_ENV['STALE'] = $_GET['stale'] = $_POST['stale'] = $_COOKIE['stale'] = $_REQUEST['stale'] = $_SERVER['STALE'] = 1;
    chdir('/');
    umask(077);
    setlocale(LC_ALL, 'C.UTF-8');
    date_default_timezone_set('Asia/Tokyo');
    mt_srand(42);
    if (function_exists('mb_internal_encoding')) {
        mb_internal_encoding('ISO-8859-1');
    }
    Holder::$held[] = fopen(__FILE__, 'r');
    $GLOBALS['stale_closure'] = fn() => 1;
    ob_start();   // left open
    echo "";
}

function act(string $mode): void
{
    global $dir;
    switch ($mode) {
        case 'normal': break;
        case 'exit': exit();
        case 'exit_int': exit(3);
        case 'exit_string': exit("bye\n");
        case 'die': die("dead\n");
        case 'exception': throw new Exception('boom');
        case 'type_error': strlen([]);
        case 'parse_error': file_put_contents("$dir/parse.inc", '<?php this is not php;'); include "$dir/parse.inc"; break;
        case 'included_error': file_put_contents("$dir/error.inc", '<?php undefined_function_in_include();'); include "$dir/error.inc"; break;
        case 'redeclare': file_put_contents("$dir/redeclare.inc", '<?php function already_declared() {}'); include "$dir/redeclare.inc"; break;
        case 'compile_error': file_put_contents("$dir/compile.inc", '<?php class K { abstract function f(); }'); include "$dir/compile.inc"; break;
        case 'memory': ini_set('memory_limit', '8M'); $a = []; while (true) { $a[] = str_repeat('x', 1 << 20); }
        case 'timeout': set_time_limit(1); while (true) {}
        case 'recursion': ini_set('memory_limit', '32M'); $f = function () use (&$f) { $f(); }; $f();
        case 'stack_overflow': $f = function ($x) use (&$f) { return array_map($f, [$x]); }; $f(1); break;
        case 'user_error': trigger_error('fatal', E_USER_ERROR); break;
        case 'error_to_exception': set_error_handler(function ($n, $s) { throw new ErrorException($s); }); echo $undefined; break;
        case 'exception_handler': set_exception_handler(function ($e) { echo "handled: ", $e->getMessage(), "\n"; }); throw new Exception('boom'); break;
        case 'destructor_exception': $GLOBALS['thrower'] = new Thrower; break;
        case 'fiber_exception': (new Fiber(function () { throw new Exception('in a fiber'); }))->start(); break;
        case 'fiber_suspended':
            $f = new Fiber(function () { $x = new class { function __destruct() { echo "fiber object destroyed\n"; } }; try { Fiber::suspend(); } finally { echo "fiber finally\n"; } });
            $f->start();
            $GLOBALS['suspended'] = $f;
            Holder::$held[] = $f;
            break;
        case 'fibers_many':
            for ($i = 0; $i < 50; $i++) {
                $f = new Fiber(function () use ($i) { $s = fopen(__FILE__, 'r'); try { Fiber::suspend(); } finally { if ($i == 49) { echo "last fiber finally\n"; } } });
                $f->start();
                Holder::$held[] = $f;
            }
            break;
        default: throw new LogicException("unknown mode $mode");
    }
}

register_shutdown_function(function () use ($log) { file_put_contents($log, "shutdown\n", FILE_APPEND); });
mark($log);
echo "failing request: $mode in $ctx\n";
switch ($ctx) {
    case 'main': act($mode); break;
    case 'shutdown': register_shutdown_function(fn() => act($mode)); break;
    case 'destructor': Holder::$held[] = new Dtor($mode); break;
    case 'ob': ob_start(function ($buffer) use ($mode) { act($mode); return $buffer; }); break;
    case 'fiber': (new Fiber(fn() => act($mode)))->start(); break;
    case 'loop':
        require __DIR__ . '/loop.inc';
        $loop = new Loop;
        $loop->runAll(
            function () use ($loop) { $loop->sleep(200000); echo "never\n"; },
            function () use ($loop) { [$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0); Holder::$held[] = $GLOBALS['wake'] = $b; $loop->poller->readable($a); echo "never\n"; },
            function () use ($loop) { $loop->sleep(1000); act($GLOBALS['mode']); fwrite($GLOBALS['wake'], 'x'); },
        );
        break;
}
echo "end of the failing request\n";
