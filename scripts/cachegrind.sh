#!/usr/bin/env bash
# Simulated cache misses with valgrind's cachegrind, in an Ubuntu container.
#
# usage: scripts/cachegrind.sh [FILE] [parse|book]
#
# macOS has no perf and no valgrind, so this runs on Linux in Docker. The
# container builds its own copy (-O2 -g) in /tmp, runs `itchbook <command>`
# under cachegrind, and prints the totals plus the functions with the most
# last-level (LL) data misses. The default file is data/small.itch, because
# cachegrind runs 20 to 50 times slower than native.
#
# Cachegrind simulates a cache; it does not measure the CPU. Its L1 and LL
# sizes come from the machine it detects, and the numbers are best used to
# compare functions and versions, not as absolute time.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
file="${1:-$root/data/small.itch}"
command="${2:-book}"
# docker -v treats a relative left-hand side as a volume name, not a folder.
file="$(cd "$(dirname "$file")" && pwd)/$(basename "$file")"

docker run --rm \
  -v "$root":/src:ro \
  -v "$(dirname "$file")":/data:ro \
  -e "FILE=/data/$(basename "$file")" -e "CMD=$command" \
  ubuntu:24.04 bash -c '
set -euo pipefail
apt-get update -qq > /dev/null
apt-get install -y -qq build-essential cmake ninja-build valgrind python3 > /dev/null
cmake -S /src -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer" \
  -DITCHBOOK_BUILD_TESTS=OFF > /dev/null
cmake --build /tmp/b --target itchbook > /dev/null
cd /tmp
# Valgrind cannot detect the cache sizes under Docker on Apple silicon, so
# they are set here: roughly an M2 performance core (64 KiB 8-way L1 data,
# 128 KiB 8-way L1 instruction) with a 16 MiB 16-way last level (its shared L2).
valgrind --tool=cachegrind --cache-sim=yes \
  --I1=131072,8,64 --D1=65536,8,64 --LL=16777216,16,64 \
  --cachegrind-out-file=/tmp/cg.out \
  /tmp/b/itchbook "$CMD" "$FILE" 2>&1 | grep -vE "^==[0-9]+== (Cachegrind, a cache|Copyright|Using Valgrind|Command|$)"
echo
echo "--- functions by last-level data misses (DLmr: reads, DLmw: writes)"
cg_annotate /tmp/cg.out --show=Ir,D1mr,DLmr,DLmw --sort=DLmr 2>&1
'
