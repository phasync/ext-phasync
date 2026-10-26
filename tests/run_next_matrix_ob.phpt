--TEST--
run_next(): the next request is clean however the previous one ended -- ob
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next_matrix.inc';
rn_matrix('ob');
?>
--EXPECT--
failures: 0
