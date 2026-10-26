--TEST--
run_next(): later runs see the request info of a first run: the script path, the response code, $_SERVER armed, PHP_CLI_PROCESS_TITLE
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
// $_SERVER is never named literally in the script: the CLI arms the auto-global itself
file_put_contents("$d/info.php", <<<'PHP'
<?php
$name = '_SERVER';
$code = http_response_code();
http_response_code(500);
echo "server=", isset($GLOBALS[$name]) ? 'armed' : 'missing';
echo " file=", $GLOBALS[$name]['SCRIPT_FILENAME'] ?? '-';
echo " lastmod=", getlastmod() === filemtime(__FILE__) ? 'ok' : 'wrong';
echo " inode=", getmyinode() === fileinode(__FILE__) ? 'ok' : 'wrong';
echo " user=", get_current_user() !== '' ? 'ok' : 'empty';
echo " code=", var_export($code, true);
echo " title=", var_export(defined('PHP_CLI_PROCESS_TITLE'), true), "\n";
file_put_contents($argv[1], "x\n", FILE_APPEND);
if (count(file($argv[1])) < 3) {
    phasync\ext\run_next(__FILE__);
}
PHP);
[$out, $status] = rn_exec($d, "$d/info.php", ["$d/state"]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
server=armed file=DIR/info.php lastmod=ok inode=ok user=ok code=false title=true
server=armed file=DIR/info.php lastmod=ok inode=ok user=ok code=false title=true
server=armed file=DIR/info.php lastmod=ok inode=ok user=ok code=false title=true
status: 0
