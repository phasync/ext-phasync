--TEST--
run_next(): the next request is clean however the previous one ended -- fiber
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next_matrix.inc';
rn_matrix('fiber');
?>
--EXPECT--
failures: 0
