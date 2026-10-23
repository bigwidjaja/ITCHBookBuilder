#!/usr/bin/env bash
# Download the NASDAQ ITCH 5.0 sample day and write the 200 MiB dev subset.
# Equivalent to: zcat 12302019.NASDAQ_ITCH50.gz | head -c 200M > data/small.itch

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
data_dir="$root/data"
mkdir -p "$data_dir"

url="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"
gz="$data_dir/12302019.NASDAQ_ITCH50.gz"
small="$data_dir/small.itch"
# GNU head's 200M suffix is 200 MiB.
subset_bytes=$((200 * 1024 * 1024))

command -v curl >/dev/null || {
  echo "curl is required" >&2
  exit 1
}
command -v gzip >/dev/null || {
  echo "gzip is required" >&2
  exit 1
}

expected="$(
  curl -fsSL -I "$url" |
    awk 'tolower($1) == "content-length:" { print $2 }' |
    tr -d '\r'
)"
if [[ -z "$expected" ]]; then
  echo "could not read Content-Length for $url" >&2
  exit 1
fi

have=0
if [[ -f "$gz" ]]; then
  have="$(wc -c < "$gz" | tr -d ' ')"
fi
if [[ "$have" -eq "$expected" ]]; then
  echo "Already downloaded: $gz"
else
  echo "Downloading $url"
  curl -fL --retry 5 --retry-delay 2 -C - -o "$gz" "$url"
fi

echo "Writing $small"
# head stops at 200 MiB and closes the pipe, so gzip exits with SIGPIPE.
set +o pipefail
gzip -dc "$gz" | head -c "$subset_bytes" > "$small"
set -o pipefail

actual="$(wc -c < "$small" | tr -d ' ')"
if [[ "$actual" -ne "$subset_bytes" ]]; then
  echo "expected $subset_bytes bytes in $small, wrote $actual" >&2
  exit 1
fi

echo "Wrote $small ($actual bytes)"
