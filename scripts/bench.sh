#!/usr/bin/env bash
# Benchmarks a full-day run: Release build, several timed runs, median msg/s.
#
# usage: scripts/bench.sh [--runs N] [--cold] [--command book|parse|count] [FILE]
#
#   --command  book (default) replays the order book; parse decodes every
#              message with no book; count only frames and counts messages.
#              Comparing them splits the cost between reading, parsing and
#              the book.
#   --cold     empties the page cache before every run, so the file comes
#              from disk:
#                Linux: sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
#                macOS: sudo purge if sudo works without a password, otherwise
#                       a fresh copy written with `ditto --nocache`, which the
#                       page cache has never held
#              Warm runs (the default) follow one unmeasured run that loads
#              the file into the cache.
#
# Runs are pinned to CPU 2 with taskset where it exists (Linux). macOS cannot
# pin a process to a core, so runs there are unpinned.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runs=5
cold=0
command=book
file="$root/data/12302019.NASDAQ_ITCH50"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs) runs="$2"; shift 2 ;;
    --cold) cold=1; shift ;;
    --command) command="$2"; shift 2 ;;
    -*) echo "unknown option $1" >&2; exit 1 ;;
    *) file="$1"; shift ;;
  esac
done

cmake -S "$root" -B "$root/build-rel" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$root/build-rel" --target itchbook -j > /dev/null
bin="$root/build-rel/itchbook"

pin=()
if command -v taskset > /dev/null; then
  pin=(taskset -c 2)
fi
# ${pin[@]+"${pin[@]}"} expands to nothing when pin is empty; plain
# "${pin[@]}" is an error under set -u in the bash 3.2 that macOS ships.

copy="$(dirname "$file")/.bench-cold-copy"
trap 'rm -f "$copy"' EXIT

# Sets $input to a path whose contents are not in the page cache.
make_cold() {
  if [[ "$(uname)" == Linux ]]; then
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    input="$file"
  elif sudo -n true 2> /dev/null; then
    sync
    sudo purge
    input="$file"
  else
    rm -f "$copy"
    ditto --nocache "$file" "$copy"
    input="$copy"
  fi
}

if [[ $cold == 0 ]]; then
  ${pin[@]+"${pin[@]}"} "$bin" "$command" "$file" > /dev/null  # warm-up: fills the page cache
fi

walls=()
messages=0
for ((i = 1; i <= runs; i++)); do
  input="$file"
  if [[ $cold == 1 ]]; then
    make_cold
  fi
  out="$({ /usr/bin/time -p ${pin[@]+"${pin[@]}"} "$bin" "$command" "$input"; } 2>&1)"
  messages="$(awk '/^messages:/ { print $2 }' <<< "$out")"
  wall="$(awk '/^real/ { print $2 }' <<< "$out")"
  walls+=("$wall")
  awk -v w="$wall" -v m="$messages" -v i="$i" \
    'BEGIN { printf "run %d: %7.2f s  %5.2f M msg/s\n", i, w, m / w / 1e6 }'
done

median="$(printf '%s\n' "${walls[@]}" | sort -n |
  awk '{ v[NR] = $1 } END { print (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }')"
mode="warm"
[[ $cold == 1 ]] && mode="cold"
pinned="pinned to CPU 2"
[[ ${#pin[@]} == 0 ]] && pinned="unpinned"
awk -v w="$median" -v m="$messages" -v c="$command" -v r="$runs" -v mode="$mode" -v p="$pinned" \
  'BEGIN { printf "%s, %s, %d runs, %s: median %.2f s  %.2f M msg/s  %.0f ns/msg\n",
           c, mode, r, p, w, m / w / 1e6, w / m * 1e9 }'
