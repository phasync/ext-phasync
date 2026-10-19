#!/bin/sh
# Runs fiber-interop/ext-scope's own tests (scope/tests/*.phpt) against the copy
# bundled in phasync.so. Their --EXTENSIONS-- line names the standalone "scope"
# module, which phasync.so never registers in module_registry (see phasync.c,
# PHP_MINIT(phasync)); a throwaway copy with that line swapped for "phasync" is
# what actually runs. The scope/ subtree itself is never modified.
set -e
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp scope/tests/*.phpt scope/tests/*.inc "$tmp/" 2>/dev/null || true
for f in "$tmp"/*.phpt; do
	perl -0pi -e 's/--EXTENSIONS--\nscope\n/--EXTENSIONS--\nphasync\n/' "$f"
done
php run-tests.php -d extension="$PWD/modules/phasync.so" "$@" "$tmp"/*.phpt
