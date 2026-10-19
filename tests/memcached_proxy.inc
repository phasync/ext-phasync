<?php
// A TCP proxy in front of memcached that delays every reply, so a test can tell
// overlapping calls from sequential ones by the clock. Run as a child:
//   php memcached_proxy.inc <upstream host:port> <delay ms> [<listen port>]
// It prints the port it listens on, then serves until killed. Plain PHP, no extension.
[, $upstream, $delay] = $argv;
$delay /= 1000;
$server = stream_socket_server('tcp://127.0.0.1:' . ($argv[3] ?? 0), $errno, $errstr);
echo parse_url('tcp://' . stream_socket_get_name($server, false), PHP_URL_PORT), "\n";
$pairs = [];     // (int) stream => peer stream
$pending = [];   // [due time, destination, bytes]
while (true) {
    $read = array_merge([$server], array_values(array_filter($pairs, 'is_resource')));
    $write = $except = null;
    $timeout = $pending ? max(0, $pending[0][0] - microtime(true)) : 1;
    if (stream_select($read, $write, $except, (int) $timeout, (int) (($timeout - (int) $timeout) * 1e6)) !== false) {
        foreach ($read as $s) {
            if ($s === $server) {
                $c = stream_socket_accept($server);
                $u = stream_socket_client("tcp://$upstream");
                $pairs[(int) $c] = $c;
                $pairs[(int) $u] = $u;
                $peer[(int) $c] = $u;
                $peer[(int) $u] = $c;
                $isUpstream[(int) $u] = true;
                continue;
            }
            $data = fread($s, 65536);
            if ($data === '' || $data === false) {
                foreach ([$s, $peer[(int) $s]] as $x) { unset($pairs[(int) $x]); fclose($x); }
                continue;
            }
            if (isset($isUpstream[(int) $s])) {
                $pending[] = [microtime(true) + $delay, $peer[(int) $s], $data];   // a reply: delayed
            } else {
                fwrite($peer[(int) $s], $data);
            }
        }
    }
    while ($pending && $pending[0][0] <= microtime(true)) {
        [, $to, $data] = array_shift($pending);
        if (is_resource($to)) { fwrite($to, $data); }
    }
}
