--TEST--
stat cache: what a child process changed is seen right after exec(), shell_exec(), system(), proc_close() and pclose() return (waiting for a child is a sync point; the watcher thread may not have woken yet)
--EXTENSIONS--
phasync
--INI--
phasync.stat_cache=1
--FILE--
<?php
$d = sys_get_temp_dir() . '/phasync-sc-child-' . getmypid();
mkdir($d);
$a = "$d/a"; $b = "$d/b";
touch($a);
$q = 'escapeshellarg';
$bad = 0;
for ($i = 0; $i < 200; $i++) {
    [$from, $to] = $i % 2 ? [$b, $a] : [$a, $b];
    file_exists($from); file_exists($to);   // both cached before the child runs
    switch ($i % 5) {
        case 0: exec('mv ' . $q($from) . ' ' . $q($to)); break;
        case 1: shell_exec('mv ' . $q($from) . ' ' . $q($to)); break;
        case 2: system('mv ' . $q($from) . ' ' . $q($to)); break;
        case 3: proc_close(proc_open(['mv', $from, $to], [], $pipes)); break;
        case 4: pclose(popen('mv ' . $q($from) . ' ' . $q($to), 'r')); break;
    }
    if (file_exists($from) || !file_exists($to)) { $bad++; }
}
echo "stale answers: $bad\n";
@unlink($a); @unlink($b); rmdir($d);
?>
--EXPECT--
stale answers: 0
