#!/bin/sh
# Renders every native-lobby screen to PNG without launching the game.
#
#   tools/lobby_preview/preview.sh <game-dir> [output-dir]
#
# <game-dir> is the folder holding umvc3.exe; its UI archives are unpacked
# into build/ui_cache (ignored by git) on first use.
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 <game-dir> [output-dir]" >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
game=$1
out=${2:-$root/build/lobby_preview}
cache=$root/build/ui_cache
tool=$root/tools/umvc3_ui/umvc3_ui.py

mkdir -p "$out" "$root/build"
if [ ! -f "$cache/nrodinpro_BM_HQ_NOMIP.fsd" ]; then
    python3 "$tool" extract "$game" "$cache"
fi

"${HOST_CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -I"$root/UMVC3Hook/code" \
    "$root/tools/lobby_preview/preview.cpp" \
    "$root/UMVC3Hook/code/lobby_ui.cpp" "$root/UMVC3Hook/code/lobby_canvas.cpp" \
    -o "$root/build/lobby_preview_host"
"$root/build/lobby_preview_host" "$out"

for commands in "$out"/*.json; do
    python3 "$tool" render "$commands" "${commands%.json}.png" "$cache"
done
echo "screens written to $out"
