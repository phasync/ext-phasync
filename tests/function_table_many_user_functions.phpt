--TEST--
Many user functions in one request: the function table's rehash does not leave one of them behind at shutdown (#35, zend_mm_heap corrupted)
--EXTENSIONS--
phasync
--FILE--
<?php
// phasync removes its checkpoint() from the function table at startup. If that leaves
// a hole before the end of the table, the table's compaction (when a request fills it)
// moved the user functions one place down, below the count of persistent functions, so
// the engine did not remove the first of them at request shutdown. Its freed op_array
// was then destroyed at module shutdown: zend_mm_heap corrupted, exit status 134.
$code = '';
for ($i = 0; $i < 3000; $i++) {
    $code .= "function phasync_many_$i() { return $i; }\n";
}
eval($code);
echo phasync_many_2999(), "\n";
echo "done\n";
?>
--EXPECT--
2999
done
