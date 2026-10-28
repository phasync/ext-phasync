--TEST--
memo (#51): listings of kernel file systems are never remembered: /proc changes with no mtime change
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!is_dir('/proc/self/fd')) die('skip needs /proc'); ?>
--INI--
phasync.memo_memory=64M
--FILE--
<?php
for ($i = 0; $i < 3; $i++) $before = count(scandir('/proc/self/task'));   // listed often enough to be remembered
phasync\ext\Suspension::setPreemptInterval(1.0);                     // a thread more
var_dump(count(scandir('/proc/self/task')) - $before);
?>
--EXPECT--
int(1)
