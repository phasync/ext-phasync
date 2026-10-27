--TEST--
set_preempt_function(): a checkpoint due inside a rebound closure does not grow CG(arena) per closure (bug #30)
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/preempt.inc';
phasync\ext\preempt_main(__DIR__ . '/preempt_arena_leak.body.inc');
?>
--EXPECT--
stable
