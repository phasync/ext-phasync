--TEST--
sapi(): I/O in curl callbacks (CURLOPT_DEBUGFUNCTION, PHP 8.4+) runs inline
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('curl')) die("skip $why");
if (PHP_VERSION_ID < 80400) die('skip CURLOPT_DEBUGFUNCTION is PHP 8.4+');
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child('require ' . var_export(__DIR__ . '/c_invoked.inc', true) . ";\n" . <<<'CHILD'
function ci_file(): string { $f = tempnam(sys_get_temp_dir(), 'phasync_cu_'); file_put_contents($f, str_repeat('x', 4096)); return $f; }
c_invoked("curl debug function", function ($sleep) {
    $ch = curl_init('file://' . ci_file());
    curl_setopt_array($ch, [CURLOPT_RETURNTRANSFER => true, CURLOPT_VERBOSE => true, CURLOPT_DEBUGFUNCTION => function () use ($sleep) { static $once; if (!$once) { $once = true; $sleep(); } return 0; }]);
    curl_exec($ch);
});
CHILD, true, '');
?>
--EXPECT--
curl debug function: A before, A after, B
