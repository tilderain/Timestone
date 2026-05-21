#!/bin/sh
out="$1"
[ -z "$out" ] && out="all.txt"

# Clear output
> "$out"

find UMVC3Hook UMVC3Hook/code -maxdepth 1 -name '*.cpp' | sort | while IFS= read -r f; do
  echo "===== $f =====" >> "$out"
  cat "$f" >> "$out"
  echo "" >> "$out"
done

echo "Wrote to $out"
