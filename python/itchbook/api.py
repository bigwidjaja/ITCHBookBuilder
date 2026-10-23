"""REST API over a SnapshotIndex.

Run:  uvicorn itchbook.api:app

The index is built once at startup. Configuration comes from environment
variables:

  ITCHBOOK_FILE              ITCH file (default data/12302019.NASDAQ_ITCH50)
  ITCHBOOK_SYMBOLS           comma-separated symbols to index
                             (default QQQ,SPY,IWM,AAPL,MSFT)
  ITCHBOOK_SNAPSHOT_MINUTES  minutes of exchange time between snapshots;
                             0 means none, so every query replays from the
                             start of the day (default 5)

Endpoints:

  GET /book/{symbol}?at=HH:MM:SS&depth=5
  GET /bbo/{symbol}?start=HH:MM:SS&end=HH:MM:SS&interval=1s
  GET /stats
"""

import ctypes
import math
import os
import re
import resource
import sys
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Query

from . import SnapshotIndex

DEFAULT_FILE = "data/12302019.NASDAQ_ITCH50"
DEFAULT_SYMBOLS = "QQQ,SPY,IWM,AAPL,MSFT"
MAX_BBO_ROWS = 100_000

_state = {}


@asynccontextmanager
async def lifespan(app):
    path = os.environ.get("ITCHBOOK_FILE", DEFAULT_FILE)
    symbols = os.environ.get("ITCHBOOK_SYMBOLS", DEFAULT_SYMBOLS).split(",")
    minutes = float(os.environ.get("ITCHBOOK_SNAPSHOT_MINUTES", "5"))
    _state["index"] = SnapshotIndex(path, symbols, minutes * 60)
    _state["minutes"] = minutes
    yield
    _state.clear()


app = FastAPI(title="itchbook", lifespan=lifespan)


def parse_interval(text):
    """'1s', '500ms', '2m' or a plain number of seconds, as seconds."""
    match = re.fullmatch(r"(\d+(?:\.\d+)?)(ms|s|m)?", text)
    if not match:
        raise HTTPException(400, f"interval must look like 1s, 500ms or 2m, got {text!r}")
    value = float(match.group(1))
    seconds = {"ms": value / 1000, "m": value * 60}.get(match.group(2), value)
    if seconds <= 0:
        raise HTTPException(400, "interval must be positive")
    return seconds


def nan_to_none(x):
    """JSON has no NaN; an empty side of the book becomes null."""
    return None if math.isnan(x) else x


def memory_bytes():
    """Memory the process is using right now.

    On macOS this is the physical footprint. RSS undercounts there, because
    macOS compresses pages that have not been touched lately, and old
    snapshots are exactly that. On Linux it is RSS from /proc.
    """
    if sys.platform == "darwin":
        # proc_pid_rusage fills a struct rusage_info_v0: a 16-byte uuid, then
        # uint64 fields, of which ri_phys_footprint is the 8th.
        info = (ctypes.c_uint64 * 16)()
        libc = ctypes.CDLL(None)
        if libc.proc_pid_rusage(os.getpid(), 0, ctypes.byref(info)) == 0:
            return int(info[9])
        return 0
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    return 0


def seconds_of(clock):
    h, m, s = clock.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


@app.get("/book/{symbol}")
def book(symbol: str, at: str, depth: int = Query(5, ge=1, le=10_000)):
    try:
        bids, asks = _state["index"].book(symbol, at, depth)
    except KeyError:
        raise HTTPException(404, f"{symbol} is not indexed")
    except ValueError as e:
        raise HTTPException(400, str(e))
    return {
        "symbol": symbol,
        "at": at,
        "bids": [{"price": p, "shares": s} for p, s in bids],
        "asks": [{"price": p, "shares": s} for p, s in asks],
    }


@app.get("/bbo/{symbol}")
def bbo(symbol: str, start: str, end: str, interval: str = "1s"):
    seconds = parse_interval(interval)
    try:
        rows = (seconds_of(end) - seconds_of(start)) / seconds + 1
    except ValueError:
        raise HTTPException(400, "start and end must look like HH:MM:SS")
    if rows > MAX_BBO_ROWS:
        raise HTTPException(400, f"that is {rows:.0f} rows; the limit is {MAX_BBO_ROWS}")
    try:
        series = _state["index"].bbo(symbol, start, end, seconds)
    except KeyError:
        raise HTTPException(404, f"{symbol} is not indexed")
    except ValueError as e:
        raise HTTPException(400, str(e))
    return {
        "symbol": symbol,
        "interval_seconds": seconds,
        "rows": [
            {
                "time": series["clock"][i],
                "bid_price": nan_to_none(float(series["bid_price"][i])),
                "bid_shares": int(series["bid_shares"][i]),
                "ask_price": nan_to_none(float(series["ask_price"][i])),
                "ask_shares": int(series["ask_shares"][i]),
            }
            for i in range(len(series["time"]))
        ],
    }


@app.get("/stats")
def stats():
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    # ru_maxrss is bytes on macOS and kilobytes on Linux.
    peak_bytes = peak if sys.platform == "darwin" else peak * 1024
    return {
        **_state["index"].stats(),
        "snapshot_minutes": _state["minutes"],
        "server_memory_bytes": memory_bytes(),
        "server_peak_rss_bytes": peak_bytes,
    }
