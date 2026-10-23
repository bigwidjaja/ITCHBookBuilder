#!/usr/bin/env bash
# Profiles a full-day replay and draws a flame graph.
#
# usage: scripts/flamegraph.sh [FILE] [OUT.svg]
#
# Builds build-prof/ with -O2 -g -fno-omit-frame-pointer: frame pointers let
# the profiler walk the stack, and -g names the functions. Brendan Gregg's
# FlameGraph scripts are cloned into build-prof/FlameGraph.
#
#   Linux: perf record -g over the whole replay.
#   macOS: perf does not exist, so /usr/bin/sample samples the running replay
#          every 1 ms for SAMPLE_SECONDS (default 60), starting 5 s in.
#
# Also prints the functions with the most self time (samples where they were
# the innermost frame).

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
file="${1:-$root/data/12302019.NASDAQ_ITCH50}"
out="${2:-$root/notes/flamegraph-v1.svg}"
prof="$root/build-prof"

cmake -S "$root" -B "$prof" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer" \
  -DITCHBOOK_BUILD_TESTS=OFF > /dev/null
cmake --build "$prof" --target itchbook -j > /dev/null

fg="$prof/FlameGraph"
if [[ ! -d "$fg" ]]; then
  git clone --quiet --depth 1 https://github.com/brendangregg/FlameGraph.git "$fg"
fi

folded="$prof/stacks.folded"
if [[ "$(uname)" == Linux ]]; then
  perf record -F 999 -g -o "$prof/perf.data" -- "$prof/itchbook" book "$file" > /dev/null
  perf script -i "$prof/perf.data" | "$fg/stackcollapse-perf.pl" > "$folded"
else
  "$prof/itchbook" book "$file" > /dev/null &
  pid=$!
  sleep 5
  /usr/bin/sample "$pid" "${SAMPLE_SECONDS:-60}" 1 -file "$prof/sample.txt" > /dev/null 2>&1
  wait "$pid"
  awk -f "$fg/stackcollapse-sample.awk" "$prof/sample.txt" > "$folded"
fi

"$fg/flamegraph.pl" --title "itchbook v1: full-day replay" --width 1600 "$folded" > "$out"
echo "wrote $out"

echo "self time, top 15:"
awk '{ n = $NF; $NF = ""; k = split($0, f, ";"); leaf = f[k]; sub(/ +$/, "", leaf);
       self[leaf] += n; total += n }
     END { for (s in self) printf "%6.2f%%  %s\n", 100 * self[s] / total, s }' "$folded" |
  sort -rn | head -15
