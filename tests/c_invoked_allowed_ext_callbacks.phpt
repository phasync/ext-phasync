--TEST--
sapi(): I/O in xml_parse() handlers, SplHeap::compare() and SQLite3 callbacks suspends: their state is per object
--EXTENSIONS--
phasync
--SKIPIF--
<?php
require __DIR__ . '/db.inc';
if ($why = db_skip('xml', 'sqlite3')) die("skip $why");
?>
--FILE--
<?php
require __DIR__ . '/db.inc';
echo db_child('require ' . var_export(__DIR__ . '/c_invoked.inc', true) . ";\n" . <<<'CHILD'
class CiHeap extends SplMinHeap { static $s; function compare($a, $b): int { (self::$s)(); return $b <=> $a; } }
c_invoked("xml_parse", function ($sleep) { $p = xml_parser_create(); xml_set_element_handler($p, function () use ($sleep) { $sleep(); }, fn () => null); xml_parse($p, '<a/>', true); });
c_invoked("SplHeap::compare", function ($sleep) { CiHeap::$s = $sleep; $h = new CiHeap; $h->insert(1); $h->insert(2); });
c_invoked("SQLite3::createFunction", function ($sleep) { $db = new SQLite3(':memory:'); $db->createFunction('cif', function () use ($sleep) { $sleep(); return 1; }); $db->querySingle('select cif()'); });
CHILD, true, '');
?>
--EXPECT--
xml_parse: A before, B, A after
SplHeap::compare: A before, B, A after
SQLite3::createFunction: A before, B, A after
