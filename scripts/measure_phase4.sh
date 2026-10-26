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
  "step 8: v1, ifstream (count)|61e934b||count"
  "step 8: v1, ifstream (parse)|61e934b||parse"
  "step 8: v1, ifstream (book)|61e934b||book"
  "step 9: mmap (count)|9f5ab66||count"
  "step 9: mmap (parse)|9f5ab66||parse"
  "step 9: mmap (book)|9f5ab66||book"
  "step 10: BookV2 setup (unordered_map)|b4cf22f||book --book v2"
  "step 10.1: reserve|c2bc2ce||book --book v2"
  "step 10.4: FlatMap|a408938||book --book v2"
  "step 10.5: direct array|7793ad6||book --book v2-direct"
  "step 11.1: books by locate|f0c0064||book --book v2"
  "step 11.2-3: sorted vectors, scan|2671e8e||book --book v2"
  "step 11.3: sorted vectors, binary search|2671e8e||book --book v2-bsearch"
  "step 11.4: level pool|49c7c19||book --book v2"
  "step 11.5: 12-byte orders|9be249f||book --book v2"
  "step 12.1: virtual dispatch|7d1df51||book --book v2 --dispatch virtual"
  "step 12.1: template dispatch|7d1df51||book --book v2 --dispatch template"
  "step 12.4: -O2|f773a04|-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG|book --book v2"
  "step 12.4: -O3 (Release default)|f773a04||book --book v2"
  "step 12.4: -O3 -march=native|f773a04|-DITCHBOOK_NATIVE=ON|book --book v2"
  "step 12.4: -O3 + LTO|f773a04|-DITCHBOOK_LTO=ON|book --book v2"
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
