--TEST--
sapi(): a Fiber bound to another Scope (an event loop's service, bound to the root Scope) started inside a request isn't the request's: its output and connection status are the process's, not the ended request's (swerve#102)
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!class_exists('Fiber')) die('skip requires Fibers'); ?>
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;

function serve(Scope $req, Closure $start): array
{
    $s = new Sink; $fiber = null;
    $req->run(function () use ($s, $start, &$fiber) {
        sapi(function () use ($start, &$fiber) {
            $fiber = $start();
            $fiber->start();
            echo "response";
        }, $s);
    });
    echo "[resume]\n";
    $fiber->resume();
    return [$s->out];
}

$req = new Scope;
$body = function () {
    Fiber::suspend();
    fwrite(STDOUT, "aborted=" . connection_aborted() . "\n");
    echo "fiber echo\n";
};

echo "-- bound to the root Scope\n";
[$out] = serve($req, fn () => new Fiber(Scope::getRootScope()->bind($body)));
var_dump($out);

echo "-- bound to the request's own Scope\n";
[$out] = serve($req, fn () => new Fiber($req->bind($body)));
var_dump($out);

echo "-- unbound\n";
[$out] = serve($req, fn () => new Fiber($body));
var_dump($out);
?>
--EXPECT--
-- bound to the root Scope
[resume]
aborted=0
fiber echo
string(8) "response"
-- bound to the request's own Scope
[resume]
aborted=1
string(8) "response"
-- unbound
[resume]
aborted=1
string(8) "response"
