--TEST--
memo json_decode(..., true): a JSON text decoded before (seen twice, at any size) comes back as the cached array, copy-on-write, also in a forked process (which inherits the cache)
--EXTENSIONS--
phasync
json
--SKIPIF--
<?php if (!function_exists('pcntl_fork')) die('skip needs pcntl_fork'); ?>
--INI--
phasync.memo_memory=64M
--FILE--
<?php
$json  = json_encode(['name' => 'theme', 'list' => range(1, 300), 'nested' => ['a' => ['b' => 'c'], 'x' => 1.5, 't' => true, 'n' => null]]);
$small = json_encode(['a' => 1]);
$hits  = static fn () => phasync\ext\memo_stats()['json_hits'];

$h0 = $hits();
$first  = json_decode($json, true);   // first sight: decoded
$second = json_decode($json, true);   // second sight: decoded, and stored
$third  = json_decode($json, true);   // from the store
echo 'equal: ', var_export($first === $second && $second === $third, true), ', hits: ', $hits() - $h0, "\n";

$third['list'][] = 'changed';          // copy-on-write: only this copy changes
$third['nested']['a']['b'] = 'changed';
$again = json_decode($json, true);
echo 'store unchanged by a write: ', var_export($again === $first, true), "\n";

json_decode('{broken');
echo 'error before: ', json_last_error() !== JSON_ERROR_NONE ? 'yes' : 'no';
json_decode($json, true);
echo ', after a hit: ', json_last_error() === JSON_ERROR_NONE ? 'none' : json_last_error_msg(), "\n";

$h1 = $hits();
json_decode($json, true, 64); json_decode($json, true, 64);                        // another depth
json_decode($json, true, 512, JSON_BIGINT_AS_STRING);                               // flags
echo 'bypassed: ', $hits() - $h1, " hits\n";
$h2 = $hits();
json_decode($small, true); json_decode($small, true); json_decode($small, true);
echo 'a small text: ', $hits() - $h2, " hit\n";

$pid = pcntl_fork();
if ($pid === 0) {
    $h = $hits();
    $child = json_decode($json, true);
    exit($child === $first && $hits() - $h === 1 ? 0 : 1);
}
pcntl_waitpid($pid, $status);
echo 'forked process: ', pcntl_wexitstatus($status) === 0 ? 'hit, equal' : 'miss or different', "\n";
?>
--EXPECT--
equal: true, hits: 1
store unchanged by a write: true
error before: yes, after a hit: none
bypassed: 0 hits
a small text: 1 hit
forked process: hit, equal
