--TEST--
phasync.keywords=1: the words stay ordinary names wherever PHP accepts them today (functions, constants, methods, properties, labels, named arguments, keys), in eval() and included files too; data after __halt_compiler() is left alone
--EXTENSIONS--
phasync
--INI--
phasync.keywords=1
--FILE--
<?php
require __DIR__ . '/keywords.inc';
function run($x = 'run') { return "the function $x()"; }
function begin() { return 'the function begin()'; }
const finish = 'the constant finish';
class C {
    const begin = 'C::begin';
    public $run = '->run';
    static function finish() { return 'C::finish()'; }
    function begin(...$a) { return '->begin(' . implode(',', array_keys($a)) . ')'; }
}
enum E { case run; }
$c = new C;
echo run(), "\n", run ('spaced'), "\n", begin(), "\n", finish, "\n";
echo C::begin, " ", $c->run, " ", C::finish(), " ", $c->begin(run: 1), "\n";
echo E::run->name, " ", ['begin' => 'key'][('begin')], "\n";
goto finish;
echo "skipped\n";
finish:
echo "label\n";
echo eval('return begin fn() => "eval";'), "\n";
$file = tempnam(sys_get_temp_dir(), 'phasync_kw_');
file_put_contents($file, '<?php $x = "included"; return finish $x;');
echo include $file, "\n";
unlink($file);
echo trim(file_get_contents(__FILE__, false, null, __COMPILER_HALT_OFFSET__)), "\n";
__halt_compiler();begin bar(); finish $x;

--EXPECT--
the function run()
the function spaced()
the function begin()
the constant finish
C::begin ->run C::finish() ->begin(run)
run key
label
go(eval)
await(included)
begin bar(); finish $x;
