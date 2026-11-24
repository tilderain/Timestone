#!/bin/sh
# Timestone installer for Linux and Steam Deck.
#
#   sh TimestoneInstall.sh [game folder]
#
# Finds Ultimate Marvel vs. Capcom 3 in Steam, downloads the latest Timestone
# and installs it. The installer is a native program carried at the end of
# this file (built from tools/timestone_install_linux.cpp); this script
# unpacks it, runs it and removes it again. Nothing is run through Wine or
# Proton, and nothing is left outside the game folder.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
start=$(awk '/^__TIMESTONE_PROGRAM__$/ { print NR + 1; exit }' "$0")
[ -n "$start" ] || { echo "TimestoneInstall: this file is incomplete; download it again." >&2; exit 1; }

# Unpacked beside this script first; a temporary folder if that cannot be
# written or cannot run programs.
status=126
for folder in "$here" "${TMPDIR:-/tmp}"; do
    program=$folder/.TimestoneInstall.$$
    if ! tail -n +"$start" "$0" >"$program" 2>/dev/null || ! chmod +x "$program" 2>/dev/null; then
        rm -f "$program"
        continue
    fi
    status=0
    TIMESTONE_SCRIPT_DIR=$here "$program" "$@" || status=$?
    rm -f "$program"
    [ "$status" -eq 126 ] || break
done
if [ "$status" -eq 126 ]; then
    echo "TimestoneInstall: the installer could not be unpacked and run here; move this file to your home folder and try again." >&2
fi
exit "$status"
__TIMESTONE_PROGRAM__
