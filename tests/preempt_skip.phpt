--TEST--
set_preempt_function(): skipped in the cycle collector's destructors and in a fiber being destroyed
--EXTENSIONS--
phasync
--SKIPIF--
<?php require __DIR__ . '/timers.inc'; skip_if_coarse_timers(25); ?>
--FILE--
<?php
require __DIR__ . '/preempt.inc';
phasync\ext\preempt_main(__DIR__ . '/preempt_skip.body.inc');
?>
--EXPECT--
-- the cycle collector's destructors, in a fiber
destructor done
bool(true)
-- and outside any fiber
destructor done
bool(true)
bool(false)
-- a fiber being destroyed
finally done
bool(false)
-- called again afterwards
bool(true)
