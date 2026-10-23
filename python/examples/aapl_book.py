"""Prints the AAPL book at 10:30 and the first rows of its top-of-book DataFrame.

Usage: python python/examples/aapl_book.py [ITCH_FILE]
"""

import sys

import pandas as pd

import itchbook

path = sys.argv[1] if len(sys.argv) > 1 else "data/12302019.NASDAQ_ITCH50"

book = itchbook.replay(path, "AAPL", "10:30:00")
print(book)
bids, asks = book.top(5)
print(f"{'shares':>10} {'bid':>12} | {'ask':<12} {'shares':<10}")
for i in range(max(len(bids), len(asks))):
    left = f"{bids[i][1]:>10} {bids[i][0]:>12.4f}" if i < len(bids) else " " * 23
    right = f"{asks[i][0]:<12.4f} {asks[i][1]:<10}" if i < len(asks) else ""
    print(f"{left} | {right}")

print()
frame = itchbook.top_of_book_frame(path, "AAPL", 1.0)
print("AAPL best bid and ask every second, from the open:")
print(frame[frame.index >= pd.Timedelta(hours=9, minutes=30)].head(10))
