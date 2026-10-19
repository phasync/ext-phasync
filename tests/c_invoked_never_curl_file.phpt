--TEST--
sapi(): I/O in curl callbacks (write, read, xferinfo, progress; curl_exec and curl_multi_exec) runs inline
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('curl')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child('require ' . var_export(__DIR__ . '/c_invoked.inc', true) . ";\n" . <<<'CHILD'
function ci_file(): string { $f = tempnam(sys_get_temp_dir(), 'phasync_cu_'); file_put_contents($f, str_repeat('x', 4096)); return $f; }
c_invoked("curl write function", function ($sleep) {
    $ch = curl_init('file://' . ci_file());
    curl_setopt($ch, CURLOPT_WRITEFUNCTION, function ($ch, $d) use ($sleep) { $sleep(); return strlen($d); });
    curl_exec($ch);
});
c_invoked("curl read function", function ($sleep) {
    $ch = curl_init('file://' . ci_file());
    $sent = false;
    curl_setopt_array($ch, [CURLOPT_UPLOAD => true, CURLOPT_INFILESIZE => 3, CURLOPT_READFUNCTION => function ($ch, $fp, $len) use ($sleep, &$sent) {
        if ($sent) return '';
        $sent = true; $sleep(); return 'abc';
    }]);
    curl_exec($ch);
});
c_invoked("curl xferinfo function", function ($sleep) {
    $ch = curl_init('file://' . ci_file());
    curl_setopt_array($ch, [CURLOPT_RETURNTRANSFER => true, CURLOPT_NOPROGRESS => false, CURLOPT_XFERINFOFUNCTION => function () use ($sleep) { static $once; if (!$once) { $once = true; $sleep(); } return 0; }]);
    curl_exec($ch);
});
c_invoked("curl progress function", function ($sleep) {
    $ch = curl_init('file://' . ci_file());
    curl_setopt_array($ch, [CURLOPT_RETURNTRANSFER => true, CURLOPT_NOPROGRESS => false, CURLOPT_PROGRESSFUNCTION => function () use ($sleep) { static $once; if (!$once) { $once = true; $sleep(); } return 0; }]);
    curl_exec($ch);
});
c_invoked("curl_multi_exec write", function ($sleep) {
    $ch = curl_init('file://' . ci_file());
    curl_setopt($ch, CURLOPT_WRITEFUNCTION, function ($ch, $d) use ($sleep) { $sleep(); return strlen($d); });
    $mh = curl_multi_init();
    curl_multi_add_handle($mh, $ch);
    do { $st = curl_multi_exec($mh, $running); } while ($running && $st === CURLM_OK);
});
CHILD, true, '');
?>
--EXPECT--
curl write function: A before, A after, B
curl read function: A before, A after, B
curl xferinfo function: A before, A after, B
curl progress function: A before, A after, B
curl_multi_exec write: A before, A after, B
