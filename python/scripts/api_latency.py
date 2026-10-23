"""Measures /book latency and snapshot memory for several snapshot intervals.

For each interval it starts the API with that ITCHBOOK_SNAPSHOT_MINUTES, waits
for the index to build, then sends the same random /book queries one at a
time and records client-side latency. Every interval must return exactly the
same answers, so the script also checks that. Prints a markdown table.

Usage: python python/scripts/api_latency.py [--minutes 0 30 5 1] [--queries 100]
"""

import argparse
import json
import os
import random
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request

SYMBOLS = ["QQQ", "SPY", "IWM", "AAPL", "MSFT"]
OPEN, CLOSE = 9 * 3600 + 30 * 60, 16 * 3600  # regular market hours, in seconds


def get(url, timeout=600):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read())


def random_queries(n, seed=1):
    rng = random.Random(seed)
    queries = []
    for _ in range(n):
        t = rng.randrange(OPEN * 10**9, CLOSE * 10**9)
        s, ns = divmod(t, 10**9)
        clock = f"{s // 3600:02}:{s // 60 % 60:02}:{s % 60:02}.{ns:09}"
        queries.append((rng.choice(SYMBOLS), clock))
    return queries


def wait_until_ready(base, server, limit=900):
    deadline = time.time() + limit
    while time.time() < deadline:
        if server.poll() is not None:
            raise RuntimeError(f"server exited with {server.returncode}")
        try:
            return get(f"{base}/stats", timeout=5)
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            time.sleep(1)
    raise TimeoutError("index did not build in time")


def measure(minutes, queries, port):
    env = {**os.environ, "ITCHBOOK_SNAPSHOT_MINUTES": str(minutes)}
    server = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "itchbook.api:app", "--port", str(port),
         "--log-level", "warning"],
        env=env,
    )
    base = f"http://127.0.0.1:{port}"
    try:
        wait_until_ready(base, server)
        latencies, answers = [], []
        for symbol, clock in queries:
            start = time.perf_counter()
            answers.append(get(f"{base}/book/{symbol}?at={clock}&depth=5"))
            latencies.append(time.perf_counter() - start)
        stats = get(f"{base}/stats")
    finally:
        server.terminate()
        server.wait()
    return latencies, answers, stats


def ms(seconds):
    return f"{seconds * 1000:,.1f} ms" if seconds < 10 else f"{seconds:,.1f} s"


def mb(n):
    return f"{n / 1e6:,.0f} MB"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--minutes", type=float, nargs="+", default=[0, 30, 5, 1])
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    queries = random_queries(args.queries)
    rows, reference = [], None
    for minutes in args.minutes:
        latencies, answers, stats = measure(minutes, queries, args.port)
        if reference is None:
            reference = answers
        elif answers != reference:
            raise SystemExit(f"answers with {minutes}-minute snapshots differ from the first run")
        q = statistics.quantiles(latencies, n=100)
        rows.append((minutes, stats, statistics.median(latencies), q[98], max(latencies)))
        print(f"done: {minutes} minutes", file=sys.stderr, flush=True)

    print(f"{args.queries} random /book queries (depth 5) over {', '.join(SYMBOLS)}, "
          "09:30 to 16:00, one at a time. All intervals returned identical answers.\n")
    print("| Snapshots | Count | Snapshot memory (est.) | Server memory | Index build | p50 | p99 | max |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for minutes, stats, p50, p99, worst in rows:
        label = "none" if minutes == 0 else f"every {minutes:g} min"
        print(f"| {label} | {stats['snapshots']:,} | {mb(stats['snapshot_bytes'])} | "
              f"{mb(stats['server_memory_bytes'])} | {stats['build_seconds']:.1f} s | "
              f"{ms(p50)} | {ms(p99)} | {ms(worst)} |")


if __name__ == "__main__":
    main()
