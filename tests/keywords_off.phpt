--TEST--
phasync.keywords is off by default: the words are a syntax error, as without the extension
--EXTENSIONS--
phasync
--FILE--
<?php
var_dump(ini_get('phasync.keywords'));
try {
    eval('return begin fn() => 1;');
} catch (ParseError $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
?>
--EXPECT--
string(1) "0"
ParseError: syntax error, unexpected token "fn", expecting ";"
