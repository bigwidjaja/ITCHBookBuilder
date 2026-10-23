# Data shape

Full day: `12302019.NASDAQ_ITCH50` (December 30, 2019), streamed from the `.gz`:

```
gzip -dc data/12302019.NASDAQ_ITCH50.gz | ./build/itchbook stats /dev/stdin
```

- messages: **268,744,780** (matches Step 1)
- symbols: **8,906** (stock locates 1 to 8,906, no gaps)
- largest order reference number: **308,631,072**
- peak live orders: **1,924,078** (added in Step 4)
- busiest second: **16:00:00**, 1,013,496 messages

## What it means

- **Order refs (for Step 10).** A, F and U create 140,270,523 orders, but the numbers go up to 308,631,072, so about half the range is never used. An array indexed directly by order ref needs 308.6M slots, and each byte per slot costs about 309 MB.
- **Live orders stay small.** At most 1,924,078 orders are live at once, under 1% of the reference range. A hash table only has to hold the live ones.
- **Busiest second is the close.** 16:00:00 is the closing cross, about 1M messages in one second. The book has to keep up with that second.
- **Top symbols are index ETFs and mega-caps.** QQQ (Nasdaq-100), SPY (S&P 500), IWM (Russell 2000), their leveraged versions (TQQQ, URTY, TNA), XLK (tech sector), and AAPL, AMD, MSFT.
- **The book is mostly adds and deletes.** A + D are 86% of all messages. Most orders are added and deleted without trading: E + C (executions) are 5.8M against 118.6M adds.

## Full output

```
messages: 268744780
symbols: 8906
largest order ref: 308631072
peak live orders: 1924078
busiest second: 16:00:00 (1013496 messages)

messages per type:
  A    117145568
  D    114360997
  U     21639067
  E      5722824
  I      4024315
  X      2787676
  F      1485888
  P      1218602
  L       215161
  C        99917
  Q        17836
  Y         9013
  H         8966
  R         8906
  J           34
  S            6
  K            3
  V            1

top 10 symbols by messages:
   1. QQQ           2376743
   2. SPY           2138932
   3. IWM           1940757
   4. TQQQ          1626328
   5. AAPL          1519865
   6. AMD           1512674
   7. MSFT          1221478
   8. URTY          1125495
   9. TNA            948097
  10. XLK            943675
```

`data/small.itch` stops at 09:29, before the open, so its stats are mostly the start-of-day directory and pre-market trading, not a normal day.
