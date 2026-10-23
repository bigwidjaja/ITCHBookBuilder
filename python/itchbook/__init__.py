"""NASDAQ TotalView-ITCH 5.0 order book replay.

The engine is C++ (the ``_itchbook`` extension module). Every result comes
back as a Python or NumPy copy, never a reference into a C++ object, so it
stays valid after the replay that made it is gone.

Prices are dollars as floats (the ITCH integer divided by 10,000), times of
day are "HH:MM:SS" strings or nanoseconds since midnight, and an empty side of
the book has price NaN and 0 shares.
"""

from ._itchbook import Book, replay, replay_with_callback, top_of_book_series

__all__ = [
    "Book",
    "replay",
    "replay_with_callback",
    "top_of_book_frame",
    "top_of_book_series",
]


def top_of_book_frame(path, symbol, interval):
    """top_of_book_series as a pandas DataFrame indexed by time of day.

    pandas is imported here, not at the top, so ``import itchbook`` works
    without it.
    """
    import pandas as pd

    frame = pd.DataFrame(top_of_book_series(path, symbol, interval))
    frame["time"] = pd.to_timedelta(frame["time"], unit="ns")
    return frame.set_index("time")
