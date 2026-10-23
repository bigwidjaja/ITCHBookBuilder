"""Builds small ITCH files with struct.pack, for tests that need no real data."""

import struct


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
    """Writes messages, each with its 2-byte big-endian length prefix."""
    with open(path, "wb") as f:
        for m in messages:
            f.write(struct.pack(">H", len(m)) + m)
    return path


def tiny_day(path):
    """One AAPL day: three adds and a delete between 09:30 and 09:33."""
    return write_itch(
        path,
        [
            stock_directory(1, "AAPL", "09:00:00"),
            add(1, 1, "B", 100, 1_000_000, "AAPL", "09:30:00"),
            add(1, 2, "S", 50, 1_010_000, "AAPL", "09:31:00"),
            add(1, 3, "B", 10, 1_005_000, "AAPL", "09:32:00"),
            delete(1, 1, "09:33:00"),
        ],
    )
