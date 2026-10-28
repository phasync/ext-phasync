#!/bin/sh
# Issue #9: phasync via an FPM pool's php_admin_value[extension] (php_dl() in the worker).
# Expected: the worker logs "phasync cannot be loaded after startup" and exits 255; with
# -d extension= on the master the request is served. Needs php-fpm and cgi-fcgi.
# Usage: tools/fpm-dl-repro.sh [php-fpm binary] [port]
set -e
FPM=${1:-$(ls /usr/sbin/php-fpm* | head -1)}; PORT=${2:-9418}
SO=$(cd "$(dirname "$0")/.." && pwd)/modules/phasync.so
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
echo '<?php echo extension_loaded("phasync") ? "phasync loaded\n" : "not loaded\n";' > "$D/i.php"
run() { # $1: pool extra line, rest: master args
	printf '[global]\nerror_log=%s/log\n[www]\nlisten=127.0.0.1:%s\npm=static\npm.max_children=1\ncatch_workers_output=yes\n%s\n' "$D" "$PORT" "$1" > "$D/c"
	shift; : > "$D/log"
	"$FPM" -n -F -y "$D/c" -p "$D" "$@" & pid=$!; sleep 1
	SCRIPT_FILENAME=$D/i.php REQUEST_METHOD=GET timeout 3 cgi-fcgi -bind -connect 127.0.0.1:$PORT | tail -1 || true
	kill $pid; wait $pid 2>/dev/null || true
	grep -m2 -E 'Fatal|exited|signal' "$D/log" || true
}
echo "== php_admin_value[extension]"; run "php_admin_value[extension]=$SO"
echo "== -d extension= on the master"; run "" -d extension="$SO"
