#!/bin/sh
# Builds fiber-interop/ext-scope standalone (phpize, outside this tree) and checks
# that phasync.so yields to it instead of bundling its own copy, in both load
# orders: no warning, Scope works, phpinfo says "provided by the scope
# extension", and a Fiber created inside Scope::run() belongs to that scope.
set -e
cd "$(dirname "$0")/.."
root="$PWD"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# A plain cp would also carry over scope/scope.lo and scope/.libs/scope.o if
# this tree's own phasync build already ran: phasync compiles scope.c as one of
# ITS objects (COMPILE_DL_PHASYNC, no get_module), and make would then see that
# stale object as up to date and link it into the "standalone" scope.so,
# producing a library with no get_module ("Invalid library"). git's tracked
# files are always the pristine source, regardless of local build leftovers.
mkdir -p "$tmp/scope-build"
git -C scope ls-files -z | tar -C scope --null -T - -cf - | tar -C "$tmp/scope-build" -xf -
(cd "$tmp/scope-build" && phpize >/dev/null && ./configure >/dev/null && make -j"$(nproc)" >/dev/null)
standalone="$tmp/scope-build/modules/scope.so"
bundled="$root/modules/phasync.so"

check() {
	desc=$1; first=$2; second=$3
	out=$(php -d extension="$first" -d extension="$second" -r '
		$s = new Scope();
		$ok = $s->run(function () use ($s) {
			$f = new Fiber(function () {});
			$f->start();
			return Scope::getCurrent() === $s;
		});
		echo $ok ? "scope-ok\n" : "scope-FAIL\n";
	' 2>&1)
	row=$(php -d extension="$first" -d extension="$second" -r 'phpinfo(INFO_MODULES);' 2>&1 | grep '^Scope => ' || true)
	echo "$desc: $out / $row"
	case "$out" in *scope-ok*) : ;; *) echo "FAILED: $desc: $out"; exit 1 ;; esac
	case "$row" in *"provided by the scope extension"*) : ;; *) echo "FAILED: $desc: unexpected row: $row"; exit 1 ;; esac
}

check "scope then phasync" "$standalone" "$bundled"
check "phasync then scope" "$bundled" "$standalone"
echo "OK: both load orders yield to the standalone scope extension"
