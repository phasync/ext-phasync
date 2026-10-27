--TEST--
run_next(): 100 lifecycles using files, sockets, pipes, processes, curl and the Poller keep the descriptor count and the memory flat
--EXTENSIONS--
phasync
--SKIPIF--
<?php if (!in_array(PHP_OS_FAMILY, ['Linux', 'BSD', 'Solaris'], true)) die('skip run_next() needs an ELF platform'); ?>
--FILE--
<?php
require __DIR__ . '/run_next.inc';
$d = rn_dir();
file_put_contents("$d/main.php", '<?php
require ' . var_export(__DIR__ . '/loop.inc', true) . ';
$state = __DIR__ . "/state";
$s = is_file($state) ? json_decode(file_get_contents($state), true) : ["n" => 0, "log" => []];
$s["n"]++;
$files = [fopen(__FILE__, "r"), tmpfile(), fopen("php://memory", "w+")];
$pair = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
$proc = proc_open(["true"], [1 => ["pipe", "w"]], $pipes);
$loop = new Loop;
$loop->runAll(function () use ($loop, $pair) {
    $loop->go(function () use ($pair) { fwrite($pair[1], "x"); });
    $loop->poller->readable($pair[0]);
    fread($pair[0], 1);
});
$ch = curl_init("http://127.0.0.1:1/");
curl_setopt($ch, CURLOPT_TIMEOUT_MS, 50);
curl_exec($ch);
proc_close($proc);
$loop = null;
gc_collect_cycles();
if ($s["n"] >= 20) {
    $s["log"][] = [count(scandir("/proc/self/fd")), (int) preg_replace("/\D/", "", explode("VmRSS:", file_get_contents("/proc/self/status"))[1])];
}
file_put_contents($state, json_encode($s));
if ($s["n"] < 100) {
    phasync\ext\run_next(__FILE__);
    return;
}
$fds = array_unique(array_column($s["log"], 0));
$rss = array_column($s["log"], 1);
echo "lifecycles: ", $s["n"], "\n";
echo "descriptor counts seen: ", json_encode($fds), "\n";
echo "rss growth: ", (max($rss) - $rss[0]) < 2048 ? "flat" : (max($rss) - $rss[0]) . " kB", "\n";
');
[$out, $status] = rn_exec($d, "$d/main.php", [], null, ['load' => ['curl']]);
echo $out === '' ? '' : preg_replace('/descriptor counts seen: \[\d+\]/', 'descriptor counts seen: [N]', $out), "status: $status\n";
rn_clean($d);
?>
--EXPECT--
lifecycles: 100
descriptor counts seen: [N]
rss growth: flat
status: 0
