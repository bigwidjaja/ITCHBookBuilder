#!/usr/bin/env bash
# Measures every Phase 4 variant on the full day and prints one markdown row
# per variant, ready for BENCHMARKS.md.
#
# usage: scripts/measure_phase4.sh [--runs N] [--only LABEL_SUBSTRING] [FILE]
#
# Each row builds the commit it names in its own git worktree under
# build-measure/, so a row measures exactly the code that commit added and
# nothing later. Then it runs the row's command N times (default 3) after one
# unmeasured warm-up, and reports the median wall time and the largest peak
# memory. On macOS, peak memory is the peak physical footprint (RSS leaves out
# compressed pages); on Linux it is the maximum RSS.
#
# Free memory first: the 8.25 GB day must fit in the page cache, or the runs
# measure the disk. The script checks and warns.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runs=3
only=""
file="$root/data/12302019.NASDAQ_ITCH50"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs) runs="$2"; shift 2 ;;
    --only) only="$2"; shift 2 ;;
    -*) echo "unknown option $1" >&2; exit 1 ;;
    *) file="$1"; shift ;;
  esac
done

# label | commit | extra cmake flags | itchbook arguments after FILE ("cmd" is replaced)
rows=(
  "step 8: v1, ifstream (count)|1bd67a8||count"
  "step 8: v1, ifstream (parse)|1bd67a8||parse"
  "step 8: v1, ifstream (book)|1bd67a8||book"
  "step 9: mmap (count)|74bf94c||count"
  "step 9: mmap (parse)|74bf94c||parse"
  "step 9: mmap (book)|74bf94c||book"
  "step 10: BookV2 setup (unordered_map)|25529c6||book --book v2"
  "step 10.1: reserve|a012fcf||book --book v2"
  "step 10.4: FlatMap|d8bf907||book --book v2"
  "step 10.5: direct array|606acaa||book --book v2-direct"
  "step 11.1: books by locate|fe7acc6||book --book v2"
  "step 11.2-3: sorted vectors, scan|4b8a238||book --book v2"
  "step 11.3: sorted vectors, binary search|4b8a238||book --book v2-bsearch"
  "step 11.4: level pool|205aba5||book --book v2"
  "step 11.5: 12-byte orders|8a689d9||book --book v2"
  "step 12.1: virtual dispatch|3a4e633||book --book v2 --dispatch virtual"
  "step 12.1: template dispatch|3a4e633||book --book v2 --dispatch template"
  "step 12.4: -O2|86d3570|-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG|book --book v2"
  "step 12.4: -O3 (Release default)|86d3570||book --book v2"
  "step 12.4: -O3 -march=native|86d3570|-DITCHBOOK_NATIVE=ON|book --book v2"
  "step 12.4: -O3 + LTO|86d3570|-DITCHBOOK_LTO=ON|book --book v2"
)

size=$(wc -c < "$file" | tr -d ' ')
if [[ "$(uname)" == Darwin ]]; then
  page=$(sysctl -n hw.pagesize)
  cached=$(( $(vm_stat | awk '/File-backed pages/ { gsub(/\./, "", $3); print $3 }') * page ))
  free=$(( $(vm_stat | awk '/Pages free/ { gsub(/\./, "", $3); print $3 }') * page ))
  if (( cached + free < size )); then
    echo "warning: only $(( (cached + free) / 1000000 )) MB cached or free for a $(( size / 1000000 )) MB file;" >&2
    echo "         close apps first, or these runs will mostly measure the disk." >&2
  fi
fi

# Peak memory of one run, in bytes.
peak_of() {
  if [[ "$(uname)" == Darwin ]]; then
    awk '/peak memory footprint/ { print $1 }' "$1"
  else
    awk '/Maximum resident set size/ { print $NF * 1024 }' "$1"
  fi
}
time_flag=-l
[[ "$(uname)" == Linux ]] && time_flag=-v

echo "| Variant | Commit | Wall time (median of $runs) | msg/s | ns/msg | Peak memory |"
echo "| --- | --- | --- | --- | --- | --- |"
for row in "${rows[@]}"; do
  IFS='|' read -r label commit cmake_flags args <<< "$row"
  if [[ -n "$only" && "$label" != *"$only"* ]]; then
    continue
  fi
  tag="$(echo "$commit $cmake_flags" | tr -c 'A-Za-z0-9\n' '-')"
  src="$root/build-measure/src-$tag"
  if [[ ! -d "$src" ]]; then
    git -C "$root" worktree add --detach --quiet "$src" "$commit"
  fi
  # shellcheck disable=SC2086  # cmake_flags may hold several flags
  cmake -S "$src" -B "$src/build-rel" -DCMAKE_BUILD_TYPE=Release -DITCHBOOK_BUILD_TESTS=OFF \
    ${cmake_flags:+"$cmake_flags"} > /dev/null
  cmake --build "$src/build-rel" --target itchbook -j > /dev/null
  bin="$src/build-rel/itchbook"
  read -r -a argv <<< "$args"
  cmd="${argv[0]}"
  rest=("${argv[@]:1}")

  "$bin" "$cmd" "$file" ${rest[@]+"${rest[@]}"} > /dev/null  # warm-up
  walls=()
  peak=0
  messages=0
  log="$(mktemp)"
  for ((i = 1; i <= runs; i++)); do
    # itchbook's report goes to $log.out, /usr/bin/time's to $log.
    /usr/bin/time "$time_flag" "$bin" "$cmd" "$file" ${rest[@]+"${rest[@]}"} > "$log.out" 2> "$log"
    messages="$(awk '/^messages:/ { print $2 }' "$log.out")"
    walls+=("$(awk '/real/ { print $1; exit }' "$log")")
    p="$(peak_of "$log")"
    (( p > peak )) && peak=$p
  done
  rm -f "$log" "$log.out"
  median="$(printf '%s\n' "${walls[@]}" | sort -n |
    awk '{ v[NR] = $1 } END { print (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }')"
  awk -v l="$label" -v c="$commit" -v w="$median" -v m="$messages" -v p="$peak" \
    'BEGIN { printf "| %s | `%s` | %.2f s | %.2f M | %.0f | %.0f MB |\n", l, c, w, m / w / 1e6, w / m * 1e9, p / 1e6 }'
done
