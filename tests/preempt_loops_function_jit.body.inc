<?php
var_dump(opcache_get_status()['jit']['on']);
require __DIR__ . '/preempt_loops.inc';
?>
