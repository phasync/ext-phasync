--TEST--
memo file_get_contents($path): an unchanged file read before (seen twice, any size up to 4 MB) comes back as the cached string; a change to the file is seen at once; json_decode() of it also hits; a forked process hits too
--EXTENSIONS--
phasync
json
--SKIPIF--
<?php if (!function_exists('pcntl_fork')) die('skip needs pcntl_fork'); ?>
--INI--
phasync.memo_memory=64M
--FILE--
<?php
$file  = tempnam(sys_get_temp_dir(), 'phasync-shared-');
$data  = json_encode(['rows' => range(1, 400), 'name' => str_repeat('x', 200)]);
file_put_contents($file, $data);
$stats = static fn (string $k) => phasync\ext\memo_stats()[$k];

$h0 = $stats('file_hits');
$a = file_get_contents($file);
$b = file_get_contents($file);
$c = file_get_contents($file);
echo 'equal: ', var_export($a === $data && $b === $data && $c === $data, true), ', hits: ', $stats('file_hits') - $h0, "\n";

clearstatcache();
usleep(10000);
file_put_contents($file, strtoupper($data));   // same size, new contents
echo 'after a change: ', file_get_contents($file) === strtoupper($data) ? 'new contents' : 'STALE', "\n";
file_put_contents($file, $data);

$j0 = $stats('json_hits');
for ($i = 0; $i < 3; $i++) { $f = file_get_contents($file); $decoded = json_decode($f, true); }
echo 'json_decode(file_get_contents()): ', var_export($decoded === json_decode($data, true), true), ', json hits: ', $stats('json_hits') - $j0, "\n";

$h1 = $stats('file_hits');
$part = []; for ($i = 0; $i < 3; $i++) { $part[] = file_get_contents($file, false, null, 10, 100); }   // offset and length, as WordPress reads plugin headers
echo 'offset and length: ', var_export($part[2] === substr($data, 10, 100) && $part[0] === $part[2], true), ', hits: ', $stats('file_hits') - $h1, "\n";
$h1 = $stats('file_hits');
for ($i = 0; $i < 3; $i++) { $tail = file_get_contents($file, false, null, 50); }
echo 'offset only: ', var_export($tail === substr($data, 50), true), ', hits: ', $stats('file_hits') - $h1, "\n";
$h1 = $stats('file_hits');
file_get_contents($file, false, stream_context_create()); file_get_contents($file, false, stream_context_create());
file_get_contents('file://' . $file); file_get_contents('file://' . $file);
echo 'bypassed: ', $stats('file_hits') - $h1, " hits\n";
$tiny = tempnam(sys_get_temp_dir(), 'phasync-tiny-'); file_put_contents($tiny, '{"a":1}');
$h2 = $stats('file_hits');
file_get_contents($tiny); file_get_contents($tiny); $t = file_get_contents($tiny);
echo 'a 7-byte file: ', $stats('file_hits') - $h2, ' hit, ', $t, "\n";
unlink($tiny);

$pid = pcntl_fork();
if ($pid === 0) {
    $h = $stats('file_hits');
    exit(file_get_contents($file) === $data && $stats('file_hits') - $h === 1 ? 0 : 1);
}
pcntl_waitpid($pid, $status);
echo 'forked process: ', pcntl_wexitstatus($status) === 0 ? 'hit, equal' : 'miss or different', "\n";
unlink($file);
?>
--EXPECT--
equal: true, hits: 1
after a change: new contents
json_decode(file_get_contents()): true, json hits: 2
offset and length: true, hits: 1
offset only: true, hits: 1
bypassed: 0 hits
a 7-byte file: 1 hit, {"a":1}
forked process: hit, equal
