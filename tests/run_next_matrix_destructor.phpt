--TEST--
run_next(): the next request is clean however the previous one ended -- destructor
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next_matrix.inc';
rn_matrix('destructor');
?>
--EXPECT--
failures: 0
