# us-equity-exchange-tz-intraday-cap-01  (SCAFFOLD — BLOCKED)

## Feature under test
Engine #19 / #26 — **intraday-cap day rollover for a real-session
instrument**, where the chart timezone differs from UTC and the symbol has a
true 09:30–16:00 session with overnight + weekend gaps.

This is the deferred validation flagged when #19 was resolved: the engine
intentionally keys `max_intraday_filled_orders` / `max_intraday_loss` /
consecutive-loss rollover off `chart_timezone_` (probe-97 showed TV matched
chart tz, not exchange tz, for crypto-on-UTC+8-chart). For US equities the
serving layer pins `chart_timezone_ = America/New_York`. **No checked-in probe
exercises `chart_tz != UTC` on a real session**, so the swap-free design is
only proven a no-op on UTC crypto data.

## Why this is BLOCKED
1. **No data.** `corpus/data/` holds only ETH-USDT (24/7, UTC). This probe
   needs a real RTH **NYSE/NASDAQ intraday** feed (e.g. AAPL/SPY 1m or 5m)
   spanning multiple trading days, including overnight gaps and ideally a US
   DST transition.
2. **No TV export.** Needs a TradingView run of this strategy on that symbol
   to produce `tv_trades.csv`.

## TV / harness setup (when unblocked)
- Symbol: a liquid US equity (e.g. NASDAQ:AAPL), intraday TF (1m or 5m).
- Add the OHLCV to `corpus/data/ohlcv_<SYM>_<tf>.csv`.
- `inputs.json`:
  ```json
  {
    "engine_chart_timezone": "America/New_York",
    "runtime_overrides": {
      "timezone": "America/New_York",
      "session": "0930-1600:23456"
    }
  }
  ```
- Export "List of trades" → `tv_trades.csv`.

## Pass criterion
Identical trade list. The discriminating case: a chart-day boundary that
falls **mid-session in UTC** but at **00:00 America/New_York**. Engine entries
that bunch wrongly around UTC midnight (instead of the ET trading day) would
expose a residual rollover bug. Matching TV confirms chart_tz pinning is the
correct mechanism for US-stock intraday gates.

## Tracking
Promotion unblocks part of engine issue #26.
