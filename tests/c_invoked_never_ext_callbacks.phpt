--TEST--
sapi(): I/O in UConverter, mb_ereg_replace_callback(), ZipArchive and FFI callbacks runs inline
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('intl', 'mbstring', 'zip', 'FFI')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child('require ' . var_export(__DIR__ . '/c_invoked.inc', true) . ";\n" . <<<'CHILD'
class CiU extends UConverter { static $s; function toUCallback($reason, $source, $codeUnits, &$error) { (self::$s)(); return ''; } }
c_invoked("UConverter", function ($sleep) { CiU::$s = $sleep; (new CiU('UTF-8', 'UTF-8'))->convert("a\xffb"); });
c_invoked("mb_ereg_replace_callback", function ($sleep) { mb_ereg_replace_callback('a', function ($m) use ($sleep) { $sleep(); return 'b'; }, 'aaa'); });
c_invoked("ZipArchive", function ($sleep) {
    $f = tempnam(sys_get_temp_dir(), 'ci'); $z = new ZipArchive; $z->open($f, ZipArchive::CREATE | ZipArchive::OVERWRITE);
    $z->addFromString('a', 'x'); $z->registerProgressCallback(0.001, function () use ($sleep) { $sleep(); }); $z->close(); unlink($f);
});
c_invoked("FFI callback", function ($sleep) {
    $ffi = FFI::cdef('void qsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *));', 'libc.so.6');
    $a = FFI::new('int[2]'); $a[0] = 2; $a[1] = 1;
    $ffi->qsort($a, 2, 4, function ($x, $y) use ($sleep) { $sleep(); return 0; });
});
CHILD, true, '-d ffi.enable=1');
?>
--EXPECT--
UConverter: A before, A after, B
mb_ereg_replace_callback: A before, A after, B
ZipArchive: A before, A after, B
FFI callback: A before, A after, B
