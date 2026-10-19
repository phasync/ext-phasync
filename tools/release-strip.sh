#!/bin/sh
# Strip a release .so in place; fail if it still leaks debug info or internal symbols (#12).
# Usage: tools/release-strip.sh phasync.so   (Linux ELF or macOS Mach-O)
set -eu
so=$1
if [ "$(uname -s)" = Darwin ]; then
  strip -x "$so"
  codesign -f -s - "$so" 2>/dev/null || true   # strip invalidates the arm64 ad-hoc signature
  bad=$(nm -gU "$so" | awk '{print $NF}' | grep -v -x '_get_module' || true)
  if otool -l "$so" | grep -q -e __DWARF -e __debug; then echo "debug sections remain in $so" >&2; exit 1; fi
else
  strip --strip-unneeded "$so"
  bad=$(nm -D --defined-only "$so" | awk '{print $NF}' | grep -v -x 'get_module' || true)
  if readelf -S "$so" | grep -q -e '\.debug' -e '\.symtab'; then echo "debug/symtab sections remain in $so" >&2; exit 1; fi
fi
if [ -n "$bad" ]; then echo "unexpected exported symbols in $so:" >&2; echo "$bad" >&2; exit 1; fi
echo "release-strip ok: $so"
