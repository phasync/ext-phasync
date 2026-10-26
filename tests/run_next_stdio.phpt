--TEST--
run_next(): STDIN, STDOUT and STDERR work in every lifecycle and file descriptors 0-2 stay open and unchanged
--EXTENSIONS--
phasync
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", <<<'PHP'
<?php
$state = __DIR__ . '/state';
$n = (int) @file_get_contents($state) + 1;
file_put_contents($state, $n);
$fds = [];
foreach ([0, 1, 2] as $fd) {
    $fds[$fd] = readlink("/proc/self/fd/$fd");
}
$count = count(scandir('/proc/self/fd'));
fwrite(STDOUT, "lifecycle $n: " . (is_resource(STDIN) && is_resource(STDOUT) && is_resource(STDERR) ? 'constants ok' : 'constants broken') . "\n");
fwrite(STDERR, "lifecycle $n: stderr ok\n");
echo "fread: ", var_export(fread(STDIN, 10), true), ", eof: ", var_export(feof(STDIN), true), "\n";
if ($n > 1) {
    $prev = json_decode(file_get_contents(__DIR__ . '/fds'), true);
    echo $prev['fds'] === $fds ? "fds 0-2 unchanged\n" : "fds 0-2 CHANGED\n";
    if ($n > 2) echo $prev["count"] === $count ? "fd count flat\n" : "fd count {$prev['count']} -> $count\n";
}
file_put_contents(__DIR__ . '/fds', json_encode(['fds' => $fds, 'count' => $count]));
if ($n < 3) {
    phasync\ext\run_next(__FILE__);
}
PHP);
file_put_contents("$d/in", "hello");
[$out, $status] = rn_exec($d, "$d/main.php", [], null, ['stdin' => "$d/in"]);
echo $out, "status: $status\n";
rn_clean($d);
?>
--EXPECT--
lifecycle 1: constants ok
lifecycle 1: stderr ok
fread: 'hello', eof: false
lifecycle 2: constants ok
lifecycle 2: stderr ok
fread: '', eof: true
fds 0-2 unchanged
lifecycle 3: constants ok
lifecycle 3: stderr ok
fread: '', eof: true
fds 0-2 unchanged
fd count flat
status: 0
