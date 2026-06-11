#!/usr/bin/env python3
"""
tv_commission_crack.py — Determine TradingView's exact percent-commission rule from data.

VERDICT (spoiler):
  TV's commission formula is EXACTLY the engine's formula, in symbol currency (USDT):
      commission_USDT = 0.001 * (entry_fill_price + exit_fill_price) * qty
  The 46 "divergent" trades were never a commission discrepancy. They are account-
  currency conversion: the symbol is BINANCE:ETHUSDT.P (quote = USDT) but the TV
  account currency is USD. TV converts each trade's net PnL (and the commission
  stats) from USDT to USD using the USDTUSD daily close of the PREVIOUS UTC DAY
  relative to the trade's exit:
      NetPnL_USD = round( USDTUSD_close(utc_day(exit) - 1 day) * (gross - commission_USDT), 2 )
  Validated with Coinbase USDT-USD daily closes as a proxy for TV's rate source:
  335/336 trades match the displayed 2dp PnL EXACTLY; 1 trade (#261, |pnl|~2.08)
  is off by one cent because TV's own USDTUSD close on 2026-02-07 was <=0.999335
  vs Coinbase's 0.99937 (0.4bp source difference). All four summary aggregates
  (net profit -2006.50, commission paid 2035.71, gross profit 2830.30,
  gross loss 4836.80) reproduce exactly under this model.

Inputs:
  /tmp/eng_commslip.json                                  engine run (fills bit-exact vs TV)
  ~/Downloads/PF_4emaINT_BINANCE_ETHUSDT.P_2026-06-12_*.csv   TV list-of-trades export
  /tmp/usdtusd_daily_close.json                           cached Coinbase USDT-USD daily
                                                          closes (auto-fetched if missing)
"""
import csv
import datetime
import glob
import json
import os
import sys

UTC = datetime.timezone.utc
ENGINE_JSON = "/tmp/eng_commslip.json"
TV_CSV_GLOB = os.path.expanduser(
    "~/Downloads/PF_4emaINT_BINANCE_ETHUSDT.P_2026-06-12_*.csv")
RATES_CACHE = "/tmp/usdtusd_daily_close.json"
COMM_RATE = 0.001  # 0.1 %


# ----------------------------------------------------------------------------- data
def load_engine():
    d = json.load(open(ENGINE_JSON))
    # [entry_ms, exit_ms, entry_px, exit_px, pnl, commission, is_long]
    return d["trades"], d["all"]


def load_tv():
    paths = sorted(glob.glob(TV_CSV_GLOB))
    if not paths:
        sys.exit(f"TV csv not found: {TV_CSV_GLOB}")
    tv = {}
    with open(paths[0], encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            n = int(row["Trade number"])
            side = "exit" if row["Type"].startswith("Exit") else "entry"
            tv.setdefault(n, {})[side] = row
    return tv


def load_rates():
    """Daily USDTUSD closes keyed 'YYYY-MM-DD' (UTC). Coinbase = proxy for TV's source."""
    if os.path.exists(RATES_CACHE):
        return json.load(open(RATES_CACHE))
    import time
    import urllib.request
    out = {}
    for s, e in [("2025-03-20", "2025-10-01"),
                 ("2025-10-01", "2026-04-15"),
                 ("2026-04-10", "2026-05-05")]:
        url = ("https://api.exchange.coinbase.com/products/USDT-USD/candles"
               f"?granularity=86400&start={s}T00:00:00Z&end={e}T00:00:00Z")
        req = urllib.request.Request(url, headers={"User-Agent": "curl/8"})
        for t, lo, hi, o, c, v in json.load(urllib.request.urlopen(req, timeout=20)):
            out[datetime.datetime.fromtimestamp(t, UTC).strftime("%Y-%m-%d")] = c
        time.sleep(0.4)
    json.dump(out, open(RATES_CACHE, "w"))
    return out


def utc_day(ms, days_back=0):
    return (datetime.datetime.fromtimestamp(ms / 1000, UTC)
            - datetime.timedelta(days=days_back)).strftime("%Y-%m-%d")


def round2(x):
    """Round-half-away-from-zero to 2dp (TV display rounding)."""
    return (1 if x >= 0 else -1) * round(abs(x) + 1e-12, 2)


# ------------------------------------------------------------------- step 1: implied
def implied_commissions(trades, tv):
    """implied = gross_USDT - displayed_NetPnL  (the original, conversion-blind view)."""
    out = []
    for i, (e_ms, x_ms, e, x, pnl, comm, is_long) in enumerate(trades):
        n = i + 1
        assert abs(float(tv[n]["entry"]["Price USDT"]) - e) < 1e-9
        assert abs(float(tv[n]["exit"]["Price USDT"]) - x) < 1e-9
        gross = (x - e) if is_long else (e - x)
        out.append(gross - float(tv[n]["exit"]["Net PnL USD"]))
    return out


# ------------------------------------------------------------------- step 2: models
def run():
    trades, eng_all = load_engine()
    tv = load_tv()
    rates = load_rates()
    N = len(trades)
    assert N == 336

    imp = implied_commissions(trades, tv)
    eng = [t[5] for t in trades]
    diffs = [abs(a - b) for a, b in zip(imp, eng)]
    print("== Step 1: implied-commission dataset (conversion-blind) ==")
    print(f"  engine formula 0.001*(entry+exit): "
          f"{sum(d <= 0.006 for d in diffs)}/336 within +-0.006, "
          f"{sum(d > 0.02 for d in diffs)} decisive divergences >0.02 "
          f"(worst {max(diffs):.4f})")
    print("  -> no notional-substitution hypothesis closed the gap; the residual")
    print("     r_n = TVpnl/ENGpnl turned out to be a smooth per-day multiplier")
    print("     (same-day trades share it; |dev| up to 17.7bp on 2025-10-11).")

    print("\n== Step 2: account-currency conversion model ==")
    print("  NetPnL_USD = round( rate * (gross_USDT - 0.001*(entry+exit)), 2 )")
    print("  rate = USDTUSD daily close of utc_day(exit_fill) - 1  (Coinbase proxy)\n")

    exact, cent, fails = 0, [], []
    net = comm_total = gp = gl = 0.0
    for i, (e_ms, x_ms, e, x, pnl, comm, is_long) in enumerate(trades):
        n = i + 1
        r = rates[utc_day(x_ms, 1)]
        pred = round2(r * pnl)
        tvp = float(tv[n]["exit"]["Net PnL USD"])
        if abs(pred - tvp) < 1e-9:
            exact += 1
        elif abs(pred - tvp) <= 0.011:
            cent.append(n)
        else:
            fails.append(n)
        net += r * pnl
        comm_total += r * comm
        gp += r * pnl if pnl > 0 else 0.0
        gl -= r * pnl if pnl < 0 else 0.0

    print(f"  per-trade: {exact}/336 EXACT 2dp match, "
          f"{len(cent)} off by one cent {cent}, {len(fails)} worse {fails}")
    print(f"  aggregates (model -> TV xlsx):")
    print(f"    net profit       {net:10.2f} -> -2006.50   (engine USDT -2006.08)")
    print(f"    commission paid  {comm_total:10.2f} ->  2035.71   (engine USDT  2035.63)")
    print(f"    gross profit     {gp:10.2f} ->  2830.30")
    print(f"    gross loss       {gl:10.2f} ->  4836.80")

    print("\n  residual detail:")
    for n in cent + fails:
        e_ms, x_ms, e, x, pnl, comm, is_long = trades[n - 1]
        r = rates[utc_day(x_ms, 1)]
        tvp = float(tv[n]["exit"]["Net PnL USD"])
        lo, hi = sorted(((tvp - 0.005) / pnl, (tvp + 0.005) / pnl))
        gap = (r - hi) if r > hi else (lo - r) if r < lo else 0.0
        print(f"    #{n}: engpnl={pnl:.5f} coinbase_rate={r} pred={round2(r*pnl)} "
              f"tv={tvp}; TV's rate must lie in [{lo:.6f},{hi:.6f}] "
              f"-> {gap*1e4:.2f}bp source mismatch")

    # sanity: the old "decisive 46" all land exactly under the model
    dec = [i + 1 for i in range(N) if diffs[i] > 0.02]
    dec_ok = sum(
        abs(round2(rates[utc_day(trades[n-1][1], 1)] * trades[n-1][4])
            - float(tv[n]["exit"]["Net PnL USD"])) < 1e-9 for n in dec)
    print(f"\n  former 46 decisive trades under conversion model: {dec_ok}/{len(dec)} exact")
    print("  (incl. #160: 61.7804*1.00181 -> 61.89 displayed; "
          "#54: 61.9348*1.00058 -> 61.97 displayed)")

    print("\n== Conclusion ==")
    print("  TV percent commission = 0.001*(entry_fill + exit_fill)*qty in SYMBOL currency.")
    print("  Engine commission formula is ALREADY EXACT. The divergence is TV's")
    print("  USDT->USD account-currency conversion at previous-UTC-day USDTUSD close.")
    return 0


if __name__ == "__main__":
    sys.exit(run())
