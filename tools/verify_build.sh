#!/usr/bin/env bash
# tools/verify_build.sh — verify a release build is correct, deterministic, and distro-agnostic.
# Run inside the Nix dev shell (or any env with the x86_64-w64-mingw32-* tools on PATH).
set -euo pipefail
cd "$(dirname "$0")/.."

OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
VERSION="$(awk '/^VERSION[[:space:]]*:=/ {print $3; exit}' Makefile)"   # parsed from Makefile:25

# 1. Two clean builds must be byte-identical. The source contains no __DATE__/__TIME__/
#    __TIMESTAMP__ macros, so a deterministic rollback build must reproduce exactly.
make clean >/dev/null
make release >/dev/null
asi1=$(sha256sum Timestone.asi | cut -d' ' -f1)
exe1=$(sha256sum TimestoneUpdater.exe | cut -d' ' -f1)
make clean >/dev/null
make release >/dev/null
asi2=$(sha256sum Timestone.asi | cut -d' ' -f1)
exe2=$(sha256sum TimestoneUpdater.exe | cut -d' ' -f1)
[ "$asi1" = "$asi2" ] || { echo "FAIL: Timestone.asi not deterministic"; exit 1; }
[ "$exe1" = "$exe2" ] || { echo "FAIL: TimestoneUpdater.exe not deterministic"; exit 1; }
echo "OK: deterministic (asi=$asi1 exe=$exe1)"

# 2. PE32+ x86-64 architecture.
"$OBJDUMP" -f Timestone.asi | grep -i 'x86-64' >/dev/null || { echo "FAIL: Timestone.asi not x86-64"; exit 1; }
"$OBJDUMP" -f TimestoneUpdater.exe | grep -i 'x86-64' >/dev/null || { echo "FAIL: TimestoneUpdater.exe not x86-64"; exit 1; }
echo "OK: PE32+ x86-64"

# 3. Expected mingw import libs actually linked. d3d9 is loaded dynamically (kiero uses
#    GetModuleHandle/GetProcAddress), so it is deliberately absent from the static import
#    table; -ld3dcompiler links as D3DCOMPILER_*.dll (import lib libd3dcompiler.a).
for lib in kernel32 user32 gdi32 ws2_32; do
  "$OBJDUMP" -p Timestone.asi | grep -i "DLL Name: $lib.dll" >/dev/null || { echo "FAIL: missing import $lib.dll"; exit 1; }
done
"$OBJDUMP" -p Timestone.asi | grep -i "DLL Name: D3DCOMPILER" >/dev/null || { echo "FAIL: missing import D3DCOMPILER*.dll"; exit 1; }
echo "OK: expected imports present"

# 4. Version define reached the compiler (-DUMVC3HOOK_VERSION="$(VERSION)").
grep -aFq "$VERSION" Timestone.asi || { echo "FAIL: version $VERSION not embedded"; exit 1; }
echo "OK: version $VERSION embedded"

# 5. No Debian coupling remains.
if grep -rnE 'usr/x86_64-w64-mingw32|-win32|apt install' Makefile; then
  echo "FAIL: Debian-specific coupling remains in Makefile"; exit 1
fi
echo "OK: no Debian coupling"

# 6. Incremental rebuild is a no-op (the -MMD/-MP dependency graph is sound).
make -q release || { echo "FAIL: rebuild triggered after a clean build"; exit 1; }
echo "OK: incremental build is a no-op"

echo "ALL CHECKS PASSED"
