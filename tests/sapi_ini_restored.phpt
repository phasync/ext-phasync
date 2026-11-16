--TEST--
sapi(): a request's ini changes (ini_set, include path, error_reporting) are gone for the next request
--EXTENSIONS--
phasync
--INI--
precision=14
memory_limit=128M
--FILE--
<?php
require __DIR__ . '/sink.inc';
use function phasync\ext\sapi;
// Origin: swerve, a looping swerve.php whose handlers call ini_set().
$inc = get_include_path(); $er = error_reporting();
sapi(function () {
    ini_set('precision', '5'); ini_set('memory_limit', '77M');
    set_include_path('/nonexistent'); error_reporting(E_ERROR);
    echo ini_get('precision'), ini_get('memory_limit'), get_include_path(), error_reporting();
}, $s = new Sink);
var_dump($s->out);
sapi(function () use ($inc, $er) {
    echo ini_get('precision'), ' ', ini_get('memory_limit'), ' ', var_export(get_include_path() === $inc && error_reporting() === $er, true);
}, $s = new Sink);
var_dump($s->out);
var_dump(ini_get('precision'), ini_get('memory_limit'), get_include_path() === $inc, error_reporting() === $er);
?>
--EXPECT--
string(17) "577M/nonexistent1"
string(12) "14 128M true"
string(2) "14"
string(4) "128M"
bool(true)
bool(true)
