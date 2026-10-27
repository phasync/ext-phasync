--TEST--
Preemption: not re-entrant; a call suspended in one fiber doesn't hold back others
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/preempt.inc';
preempt_main(__DIR__ . '/preempt_reentrant.body.inc');
?>
--EXPECT--
-- a slow closure
int(1)
bool(true)
-- managed fibers preempted in turn
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "b"
  [2]=>
  string(1) "a"
}
bool(true)
bool(true)
