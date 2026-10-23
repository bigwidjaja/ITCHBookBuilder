"""Tests for the itchbook Python module.

Most tests write a tiny ITCH file with struct.pack, so they need no data.
The ones that compare against the CLI or the golden file skip when the data
or the CLI build is missing.
"""

import csv
import math
import struct
import subprocess
from pathlib import Path

import pytest

import itchbook

ROOT = Path(__file__).resolve().parents[2]
SMALL = ROOT / "data" / "small.itch"
FULL_DAY = ROOT / "data" / "12302019.NASDAQ_ITCH50"
CLI = ROOT / "build-rel" / "itchbook"
GOLDEN = ROOT / "tests" / "golden" / "golden-v1.csv"


def ns(hhmmss):
    h, m, s = (int(x) for x in hhmmss.split(":"))
    return ((h * 60 + m) * 60 + s) * 1_000_000_000


def header(kind, locate, at):
    # type, stock locate, tracking number, 6-byte timestamp
    return kind.encode() + struct.pack(">HH", locate, 0) + ns(at).to_bytes(6, "big")


def stock_directory(locate, symbol, at):
    body = symbol.ljust(8).encode() + b"QN" + struct.pack(">I", 100) + b"NCZ PN 1N" + bytes(4) + b"N"
    return header("R", locate, at) + body


def add(locate, ref, side, shares, price, symbol, at):
    body = struct.pack(">QcI", ref, side.encode(), shares) + symbol.ljust(8).encode()
    return header("A", locate, at) + body + struct.pack(">I", price)


def delete(locate, ref, at):
    return header("D", locate, at) + struct.pack(">Q", ref)


def write_itch(path, messages):
    with open(path, "wb") as f:
        for m in messages:
            f.write(struct.pack(">H", len(m)) + m)
    return path


@pytest.fixture
def tiny(tmp_path):
    return write_itch(
        tmp_path / "tiny.itch",
        [
            stock_directory(1, "AAPL", "09:00:00"),
            add(1, 1, "B", 100, 1_000_000, "AAPL", "09:30:00"),
            add(1, 2, "S", 50, 1_010_000, "AAPL", "09:31:00"),
            add(1, 3, "B", 10, 1_005_000, "AAPL", "09:32:00"),
            delete(1, 1, "09:33:00"),
        ],
    )


def test_replay_stops_at_time(tiny):
    book = itchbook.replay(tiny, "AAPL", "09:31:30")
    assert book.symbol == "AAPL"
    assert book.locate == 1
    assert book.time == "09:31:30.000000000"
    assert book.top(5) == ([(100.0, 100)], [(101.0, 50)])


def test_replay_includes_messages_at_the_time(tiny):
    bids, _ = itchbook.replay(tiny, "AAPL", "09:32:00").top(5)
    assert bids == [(100.5, 10), (100.0, 100)]


def test_top_limits_depth(tiny):
    bids, _ = itchbook.replay(tiny, "AAPL", "09:32:00").top(1)
    assert bids == [(100.5, 10)]


def test_bad_arguments(tiny, tmp_path):
    with pytest.raises(ValueError):
        itchbook.replay(tiny, "AAPL", "9:30")
    with pytest.raises(ValueError):
        itchbook.replay(tiny, "MSFT", "10:00:00")
    with pytest.raises(OSError):
        itchbook.replay(tmp_path / "missing.itch", "AAPL", "10:00:00")
    with pytest.raises(ValueError):
        itchbook.top_of_book_series(tiny, "AAPL", 0)


def test_top_of_book_series(tiny):
    s = itchbook.top_of_book_series(tiny, "AAPL", 60)
    # Samples every whole minute from 09:00 until the last message (09:33).
    assert list(s["time"][[0, -1]]) == [ns("09:00:00"), ns("09:32:00")]
    assert len(s["time"]) == 33
    at = {int(t): i for i, t in enumerate(s["time"])}

    before = at[ns("09:29:00")]
    assert math.isnan(s["bid_price"][before]) and s["bid_shares"][before] == 0

    open_ = at[ns("09:30:00")]  # the add stamped exactly 09:30:00 is included
    assert (s["bid_price"][open_], s["bid_shares"][open_]) == (100.0, 100)
    assert math.isnan(s["ask_price"][open_])

    last = at[ns("09:32:00")]
    assert (s["bid_price"][last], s["ask_price"][last]) == (100.5, 101.0)


def test_top_of_book_frame(tiny):
    pd = pytest.importorskip("pandas")
    frame = itchbook.top_of_book_frame(tiny, "AAPL", 60)
    assert list(frame.columns) == ["bid_price", "bid_shares", "ask_price", "ask_shares"]
    assert frame.loc[pd.Timedelta(minutes=9 * 60 + 31), "ask_price"] == 101.0


def test_callback_sees_every_message(tiny):
    seen = []
    itchbook.replay_with_callback(tiny, lambda kind, at, locate: seen.append((kind, at, locate)))
    assert [kind for kind, _, _ in seen] == ["R", "A", "A", "A", "D"]
    assert seen[1] == ("A", ns("09:30:00"), 1)


def test_callback_errors_propagate(tiny):
    def boom(*_):
        raise KeyError("stop")

    with pytest.raises(KeyError):
        itchbook.replay_with_callback(tiny, boom)


def cli_book(path, symbol, at, depth):
    """Runs `itchbook book` and parses its table into (bids, asks)."""
    out = subprocess.run(
        [CLI, "book", path, "--symbol", symbol, "--at", at, "--depth", str(depth)],
        capture_output=True, text=True, check=True,
    ).stdout.splitlines()
    bids, asks = [], []
    for line in out[2:]:  # skip the title and the column names
        left, right = line.split("|")
        if left.split():
            shares, price = left.split()
            bids.append((price, int(shares)))
        if right.split():
            price, shares = right.split()
            asks.append((price, int(shares)))
    return bids, asks


@pytest.mark.skipif(not (SMALL.exists() and CLI.exists()), reason="needs data/small.itch and build-rel")
@pytest.mark.parametrize("symbol,at", [("AAPL", "09:29:00"), ("QQQ", "09:00:00"), ("ARGX", "04:00:01")])
def test_matches_cli(symbol, at):
    bids, asks = itchbook.replay(SMALL, symbol, at).top(5)
    as_text = lambda side: [(f"{p:.4f}", s) for p, s in side]
    assert (as_text(bids), as_text(asks)) == cli_book(SMALL, symbol, at, 5)


@pytest.mark.skipif(not FULL_DAY.exists(), reason="needs the full-day file")
def test_series_matches_golden_file():
    """Every minute of AAPL's best bid and ask, against the v1 golden file."""
    s = itchbook.top_of_book_series(FULL_DAY, "AAPL", 60)
    rows = [r for r in csv.DictReader(open(GOLDEN)) if r["symbol"] == "AAPL"]
    assert len(rows) == len(s["time"])

    def cell(price, shares):
        return ("", "") if math.isnan(price) else (f"{price:.4f}", str(shares))

    for i, row in enumerate(rows):
        assert row["time"] == itchbook_clock(s["time"][i])
        assert (row["bid_price_1"], row["bid_shares_1"]) == cell(s["bid_price"][i], s["bid_shares"][i])
        assert (row["ask_price_1"], row["ask_shares_1"]) == cell(s["ask_price"][i], s["ask_shares"][i])


def itchbook_clock(t):
    seconds = int(t) // 1_000_000_000
    return f"{seconds // 3600:02}:{seconds // 60 % 60:02}:{seconds % 60:02}"
