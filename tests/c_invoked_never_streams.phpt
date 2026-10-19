--TEST--
sapi(): I/O in user stream wrappers during include, url_stat, opendir and in user stream filters runs inline
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/c_invoked.inc';
class CiW { public $context; static $s; static $at; private $p = 0;
  function stream_open($p, $m, $o, &$op) { if (self::$at == 'open') (self::$s)(); return true; }
  function stream_read($n) { if (self::$at == 'read') (self::$s)(); $r = $this->p ? '' : '<?php return 1;'; $this->p = 1; return $r; }
  function stream_eof() { return $this->p > 0; }
  function stream_stat() { return []; }
  function stream_set_option($o, $a, $b) { return false; }
  function url_stat($p, $f) { (self::$s)(); return false; }
  function dir_opendir($p, $o) { (self::$s)(); return true; }
  function dir_readdir() { return false; }
}
stream_wrapper_register("ciw", "CiW");
class CiF extends php_user_filter { static $s; function filter($in, $out, &$consumed, bool $closing): int {
  while ($b = stream_bucket_make_writeable($in)) { $consumed += $b->datalen; stream_bucket_append($out, $b); } (self::$s)(); return PSFS_PASS_ON; } }
stream_filter_register("cif", "CiF");
c_invoked("stream_open during include", function ($sleep) { CiW::$s = $sleep; CiW::$at = 'open'; include "ciw://a"; });
c_invoked("stream_read during include", function ($sleep) { CiW::$s = $sleep; CiW::$at = 'read'; include "ciw://b"; });
c_invoked("url_stat", function ($sleep) { CiW::$s = $sleep; file_exists("ciw://c"); });
c_invoked("dir_opendir", function ($sleep) { CiW::$s = $sleep; opendir("ciw://d"); });
c_invoked("user filter", function ($sleep) { CiF::$s = $sleep; $f = fopen("php://memory", "w+"); stream_filter_append($f, "cif", STREAM_FILTER_WRITE); fwrite($f, "x"); });
?>
--EXPECT--
stream_open during include: A before, A after, B
stream_read during include: A before, A after, B
url_stat: A before, A after, B
dir_opendir: A before, A after, B
user filter: A before, A after, B
