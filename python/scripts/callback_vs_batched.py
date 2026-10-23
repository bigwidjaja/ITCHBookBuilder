"""Times a per-message Python callback against the batched replay.

Both maintain the same BookV1 over the whole file. The batched call stays in
C++ (with the GIL released); the callback version crosses into Python once per
message. The CLI time is the baseline for both.

Usage: python python/scripts/callback_vs_batched.py [ITCH_FILE] [REPEATS]
"""

import statistics
import subprocess
import sys
import time

import itchbook

path = sys.argv[1] if len(sys.argv) > 1 else "data/small.itch"
repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 3
cli = "build-rel/itchbook"


def timed(fn):
    start = time.perf_counter()
    fn()
    return time.perf_counter() - start


def median_of(fn):
    return statistics.median(timed(fn) for _ in range(repeats))


calls = 0


def count_only(kind, at, locate):
    """The cheapest useful callback: one global increment."""
    global calls
    calls += 1


per_type = {}
per_stock = {}


def count_by_type_and_stock(kind, at, locate):
    """What a user might really do: tally messages by type and by stock."""
    per_type[kind] = per_type.get(kind, 0) + 1
    per_stock[locate] = per_stock.get(locate, 0) + 1


cli_s = median_of(lambda: subprocess.run([cli, "book", path], check=True, capture_output=True))
batched_s = median_of(lambda: itchbook.replay(path, "AAPL", "23:59:59"))
counter_s = median_of(lambda: itchbook.replay_with_callback(path, count_only))
tally_s = median_of(lambda: itchbook.replay_with_callback(path, count_by_type_and_stock))
messages = calls // repeats


def line(name, seconds):
    print(f"{name:<34} {seconds:8.3f} s  {messages / seconds / 1e6:6.2f} M msg/s  "
          f"{seconds / batched_s:5.1f}x batched  "
          f"{(seconds - batched_s) / messages * 1e9:5.0f} ns/msg over batched")


print(f"file: {path}, {messages:,} messages, median of {repeats} runs\n")
line("CLI `itchbook book`", cli_s)
line("itchbook.replay (batched)", batched_s)
line("callback: count only", counter_s)
line("callback: tally by type and stock", tally_s)
