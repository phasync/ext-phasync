--TEST--
run_next(): every run starts like an FPM request: statics, globals, declarations, constants, ini, handlers, output buffers and shutdown functions are reset
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/reset.php", <<<'PHP'
<?php
function declared_once() { static $n = 0; return ++$n; }   // redeclaring it would be fatal
class C { public static int $s = 0; }
interface I {}
echo "static=", declared_once(), " prop=", ++C::$s;
echo " global=", isset($g) ? 'set' : 'unset';
echo " ini=", ini_get('precision');
echo " const=", defined('FOO') ? 'set' : 'unset';
echo " ob=", ob_get_level();
echo " handler=", var_export(set_error_handler(fn() => true), true);
echo " included=", count(get_included_files()), "\n";
$g = 1;
ini_set('precision', '5');
define('FOO', 1);
ob_start();   // left open: flushed by the shutdown
register_shutdown_function(function () { echo "shutdown\n"; });
file_put_contents($argv[1], "x\n", FILE_APPEND);
if (count(file($argv[1])) < 3) {
    phasync\ext\run_next(__FILE__);
}
PHP);
[$out, $status] = rn_exec($d, "$d/reset.php", ["$d/state"]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
static=1 prop=1 global=unset ini=14 const=unset ob=0 handler=NULL included=1
shutdown
static=1 prop=1 global=unset ini=14 const=unset ob=0 handler=NULL included=1
shutdown
static=1 prop=1 global=unset ini=14 const=unset ob=0 handler=NULL included=1
shutdown
status: 0
