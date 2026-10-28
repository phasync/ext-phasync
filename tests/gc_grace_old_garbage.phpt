--TEST--
phasync.gc_grace: cycles that were seen by a collection and aged while live are collected as soon as they become garbage
--EXTENSIONS--
phasync
--INI--
phasync.gc_grace=200
--FILE--
<?php
require __DIR__ . "/gc_grace.inc";
$keep = [];
gc_cycles(1000, $keep);
gc_line("live", gc_collect_cycles());
usleep(300000);
$keep = null;
gc_line("dropped when old", gc_collect_cycles());
// live young data under hidden objects keeps its contents
$keep = [];
gc_cycles(10, $keep);
$keep[0]->o->o->pad[] = 4;
gc_line("young live", gc_collect_cycles());
var_dump(count($keep), $keep[0]->o->o === $keep[0], $keep[0]->pad);
usleep(300000);
gc_line("old live", gc_collect_cycles());
var_dump($keep[9]->o->o === $keep[9]);
?>
--EXPECT--
live: freed 0, gone 0, held 2000
dropped when old: freed 2000, gone 2000, held 0
young live: freed 0, gone 2000, held 20
int(10)
bool(true)
array(4) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(3)
  [3]=>
  int(4)
}
old live: freed 0, gone 2000, held 0
bool(true)
