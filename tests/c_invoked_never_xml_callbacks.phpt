--TEST--
sapi(): I/O in XSL/DOMXPath PHP-function callbacks runs inline
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('xsl', 'dom')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child('require ' . var_export(__DIR__ . '/c_invoked.inc', true) . ";\n" . <<<'CHILD'
class CiX { static $s; }
function ci_f() { (CiX::$s)(); return 'x'; }
c_invoked("XSLTProcessor", function ($sleep) {
    CiX::$s = $sleep;
    $xsl = new DOMDocument;
    $xsl->loadXML('<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:php="http://php.net/xsl"><xsl:template match="/"><xsl:value-of select="php:function(\'ci_f\')"/></xsl:template></xsl:stylesheet>');
    $p = new XSLTProcessor; $p->registerPHPFunctions(); $p->importStylesheet($xsl);
    $p->transformToXml(new DOMDocument);
});
c_invoked("DOMXPath", function ($sleep) {
    CiX::$s = $sleep;
    $d = new DOMDocument; $d->loadXML('<a/>');
    $x = new DOMXPath($d); $x->registerNamespace('php', 'http://php.net/xpath'); $x->registerPhpFunctions('ci_f');
    $x->evaluate('php:function("ci_f")');
});
CHILD, true, '');
?>
--EXPECT--
XSLTProcessor: A before, A after, B
DOMXPath: A before, A after, B
