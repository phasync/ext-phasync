--TEST--
phasync.keywords=1 under opcache: the rewrite runs beneath it, so a cached script runs rewritten without being rewritten again
--EXTENSIONS--
phasync
opcache
--INI--
phasync.keywords=1
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/keywords.inc';
$file = tempnam(sys_get_temp_dir(), 'phasync_kwo_') . '.php';
file_put_contents($file, '<?php return begin fn() => "cached";');
touch($file, time() - 10);
echo include $file, "\n";
var_dump(opcache_is_script_cached($file));
echo include $file, "\n";
unlink($file);
?>
--EXPECT--
go(cached)
bool(true)
go(cached)
