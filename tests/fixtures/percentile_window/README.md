# ta.percentile_* and ta.percentrank from the first bar, as TradingView computes them (lane TAIL-H)

TradingView keeps the window of `ta.percentile_nearest_rank` and
`ta.percentile_linear_interpolation` in one array across bars:

- The array is na until `length` values are held (bar `length - 1` of the
  run is the first with a value).
- Once `length` values are held, each new bar removes the value `length`
  bars back at its first occurrence in the array (an na value removes the
  first na), and inserts the new value before the first held element greater
  than it. The held non-na values therefore stay ascending; an na value,
  greater than nothing and with nothing greater than it, enters last, and a
  value later inserted after all the non-na values before it lands behind
  that na. The array is never re-sorted, so where its na values stand depends
  on the order the values arrived in.
- `ta.percentile_nearest_rank(source, n, p)` reads element
  `ceil(p / 100 * n) - 1`, clamped to `[0, n - 1]`; an na element reads na.
- `ta.percentile_linear_interpolation(source, n, p)` reads the array at
  `k = p / 100 * n - 0.5`, clamped to `[0, n - 1]`: element `n - 1` at
  `k = n - 1`, else `a[lo] * (1 - f) + a[lo + 1] * f` with `lo = floor(k)`
  and `f = k - lo`, na if either element is na, even at `f = 0`.
- `ta.percentrank(source, n)` is na until `n` values precede the bar, then
  counts the preceding `n` values at most the current one over `n`: an na
  current value and an all-na lookback count none (0).

A comparison with na is false throughout. The engine's
`ta::PercentileWindow` (`src/ta_misc.cpp`) keeps the same array.

Each tape is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json` and `meta.json`, run on BINANCE:BTCUSDT 15 from
2025-04-01. Each probe alternates two positions and closes one on every chart
bar; each exit comment spells the readings of the bar before the fill (`n`
for na, numbers to four decimals). The sources are price-free functions of
`bar_index`: `v = (bar_index * 7919) % 101`; `g` is `v` but na on every
`bar_index % 4 == 1`; `h` is `v` but na where `bar_index * 13 % 7 == 0`; `w`
is `v` but na on bars 0 .. 6; `q` is `v` but na on bars 0 .. 11.

- `tailh-pct-warmup-btc15` (2025-04-01 .. 04-05): per source `v`, `g` and
  `w`, nearest rank 50 and 20, linear interpolation 50 and 30 (length 10) and
  `ta.percentrank(x, 10)`; then `ta.dmi(14, 14)`'s ADX, its nearest rank 50
  and linear interpolation 50 of length 300 and its `ta.percentrank` of 300
  (ADX's own 27-bar na warmup at the front of the 300 window). ` @...`
  carries bar 0's reading, kept in a `var`.
- `tailh-pct-fullwin-btc15` (2025-04-01 .. 04-03): nearest rank 5, 15, .., 95
  of length 10 of `g` and of `h`, which read the whole array; nearest rank 30
  and 70 of `v`; `ta.percentrank` of `q` and `g`.
- `tailh-pct-edges-btc15` (2025-04-01 .. 04-05): linear interpolation 25, 45,
  75 (`k` on an element), 0 and 100, nearest rank 0 and 100 of length 10 of
  `g`; nearest rank 50, linear interpolation 50 and nearest rank 10 of length
  100 of `g`; nearest rank 50, linear interpolation 33 and nearest rank 90 of
  length 70 of `h`.

`bars.inc` holds the chart's 384 bars from the lab lane btcusdt-15's chart
feed; `ta::DMI` on them reproduces every ADX reading of the first tape.

`tests/test_ta_indicators_extras.cpp` (`test_percentile_window_tapes`)
replays the three tapes through `ta::PercentileNearestRank`,
`ta::PercentileLinearInterpolation` and `ta::PercentRank`, one instance per
Pine call site.

| tape | range | trades | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---|---:|---|---|
| `tailh-pct-warmup-btc15` | 2025-04-01 .. 2025-04-05 | 384 | `4f81492241704c6fab8fcb8bd749b2e239a58ad57c53dfb664f90eb049714599` | `5cd7d6758895` |
| `tailh-pct-fullwin-btc15` | 2025-04-01 .. 2025-04-03 | 192 | `50f3e71ad780cb10b2f3b2756079eb07af68fd4de6d171737dbe06d24e5e7363` | `7de8569531b7` |
| `tailh-pct-edges-btc15` | 2025-04-01 .. 2025-04-05 | 384 | `035f6ab27bf8eacf6ede0a157a0e1ff1b5331656899444749c6c41932b72f22a` | `e86022bf3e87` |
