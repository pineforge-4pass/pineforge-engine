#!/usr/bin/env python3
"""
tv_equity_crack.py — Crack TradingView's equity-panel conventions from data.

Strategy composite-4emarsi-integration-01, BINANCE:ETHUSDT.P 15m, comm 0.1%,
slippage 2 ticks, qty 1, capital 1,000,000 USD (account ccy USD, symbol ccy
USDT). Reference: PF_4emaINT_BINANCE_ETHUSDT.P_2026-06-12_b9188.xlsx.

VERDICT (all four targets reproduced, see run output):

  1. SHARPE -13.787
     = (mean(r) - 0.02/12) / pop_stddev(r), NOT annualized, where r are
     monthly simple returns of the month-end REALIZED equity in USD
     (initial capital + cumulative closed-trade net PnL converted USDT->USD
     at previous-UTC-day USDTUSD close), months bucketed in UTC, baseline
     1,000,000. n=14 (Apr 2025 .. May 2026). Open profit is EXCLUDED from
     month-end equity. Reproduced: -13.7873.
     Engine reports -13.5932 (sample stddev, NY-tz months, USDT marked
     equity) — three convention deltas: ddof, month tz, realized-vs-marked.

  2. MAX/AVG RUN-UP & DRAWDOWN (close-to-close) 39.20 / 29.48 / 2068.23,
     durations 9 days / 374 days
     "Close-to-close" = TRADE-close-to-TRADE-close: the equity series is
     realized USD equity sampled ONLY at trade exits (first point = first
     trade's close). TV splits that polyline into alternating phases at its
     GLOBAL maximum and GLOBAL minimum (3 phases here):
        run-up  #1: series start  -> global max   = 19.7647   (12.86 d)
        drawdown  : global max    -> global min   = 2068.2365 (374.55 d)
        run-up  #2: global min    -> series end   = 39.1959   (5.77 d)
     Phase value = endpoint-to-endpoint net change; max run-up = 39.20,
     avg run-up = (19.76+39.20)/2 = 29.48, avg duration = floor(9.32) = 9 d,
     avg drawdown = max drawdown = 2068.23, duration floor(374.55) = 374 d.
     (Local dips inside a phase are NOT separate phases — that is why avg
     drawdown == max drawdown.) Classic cummax drawdown coincides here
     (2068.23) but classic cummin run-up would give 260.22 — TV's 39.20 is
     only explained by the global-extreme phase rule.

  3. MAX DRAWDOWN (intrabar) 2072.41 / MAX RUN-UP (intrabar) 285.65
     TV keeps two curves:
       settled(t):  realized USD equity steps + an ENTRY-COMMISSION dip at
                    each entry fill (equity drops by 0.001*entry_px the
                    moment a position opens; no mark-to-market otherwise).
       excursion events per trade n (USD, converted at exit-day rate):
                    hi_n = cum_{n-1} + (gross MFE - entry_comm)
                    lo_n = cum_{n-1} + (-gross MAE - entry_comm)
                    (== TV's per-trade Favorable/Adverse excursion columns)
     Max drawdown (intrabar) = max_n [ runmax(settled before n) - lo_n ]
     Max run-up   (intrabar) = max_n [ hi_n - runmin(settled before n) ]
     i.e. the *measured* extreme is intrabar, the *reference* extreme is the
     settled curve. Reproduced exactly: 2072.41 (peak = realized +22.5366 on
     2025-04-16, trough = lo of trade 331 on 2026-04-26) and 285.65 (trough
     = equity right after trade 205's entry fill on 2025-12-09, cum204 -
     3.12 entry commission = -1664.05; peak = hi of trade 207 = -1378.40).
     Plain runmax/runmin over the full intrabar envelope gives 2076-2077 and
     292-303 — wrong; the settled-reference rule is decisive.

  4. CAGR -0.18%
     (final/initial)^(365 / D) - 1 with D = the BACKTESTING-RANGE span
     (Mar 31 2025 20:00 -> May 1 2026 20:00 display tz = 396.0 days), on the
     USD-converted net. Gives -0.1850% -> -0.18; long -0.0776% -> -0.08,
     short -0.1073% -> -0.11 (all three displayed digits match only with
     D=396/365-day year). Engine's -0.19% uses the traded span (393.75 d).

Inputs:
  /tmp/eng_commslip.json   engine trade list (entry/exit ms, fill px, pnl,
                           comm, is_long) — fills bit-exact vs TV. Regenerate
                           with scripts/run_strategy.py if missing (see
                           README block at bottom of this docstring).
  corpus/data/ohlcv_ETH-USDT-USDT_15m_warmup6m.csv  (repo) for excursions
  /tmp/usdtusd_daily_close.json  Coinbase USDTUSD daily closes (auto-fetch)
  ~/Downloads/PF_4emaINT_..._b9188.xlsx (optional) to cross-check targets
"""
import csv
import datetime
import json
import math
import os
import sys

UTC = datetime.timezone.utc
REPO = "/Users/haoliangwen/code/pineforge-engine"
ENGINE_JSON = "/tmp/eng_commslip.json"
OHLCV_CSV = os.path.join(REPO, "corpus/data/ohlcv_ETH-USDT-USDT_15m_warmup6m.csv")
RATES_CACHE = "/tmp/usdtusd_daily_close.json"
XLSX = os.path.expanduser(
    "~/Downloads/PF_4emaINT_BINANCE_ETHUSDT.P_2026-06-12_b9188.xlsx")

INITIAL = 1_000_000.0
COMM_RATE = 0.001
RF_MONTHLY = 0.02 / 12
BACKTEST_DAYS = 396.0          # Mar 31 2025 20:00 -> May 1 2026 20:00 (display tz)

TARGETS = {
    "sharpe": -13.787, "sortino": -0.997,
    "dd_c2c": 2068.23, "dd_intra": 2072.41,
    "ru_c2c": 39.20, "ru_intra": 285.65,
    "ru_avg": 29.48, "dd_avg": 2068.23,
    "ru_dur_days": 9, "dd_dur_days": 374,
    "cagr_pct": -0.18,
}


# --------------------------------------------------------------------- inputs
def load_rates():
    if os.path.exists(RATES_CACHE):
        return json.load(open(RATES_CACHE))
    import time
    import urllib.request
    out = {}
    for s, e in [("2025-03-20", "2025-10-01"), ("2025-10-01", "2026-04-15"),
                 ("2026-04-10", "2026-05-05")]:
        url = ("https://api.exchange.coinbase.com/products/USDT-USD/candles"
               f"?granularity=86400&start={s}T00:00:00Z&end={e}T00:00:00Z")
        req = urllib.request.Request(url, headers={"User-Agent": "curl/8"})
        for t, lo, hi, o, c, v in json.load(urllib.request.urlopen(req, timeout=20)):
            out[datetime.datetime.fromtimestamp(t, UTC).strftime("%Y-%m-%d")] = c
        time.sleep(0.4)
    json.dump(out, open(RATES_CACHE, "w"))
    return out


RATES = load_rates()


def rate_prev_utc_day(ms):
    """TV converts symbol-ccy amounts at the USDTUSD close of the PREVIOUS
    UTC day (Coinbase daily closes as proxy for TV's source; 335/336 trades
    match the displayed 2dp PnL exactly)."""
    d = (datetime.datetime.fromtimestamp(ms / 1000, UTC)
         - datetime.timedelta(days=1)).strftime("%Y-%m-%d")
    return RATES[d]


def load_trades():
    # [entry_ms, exit_ms, entry_px, exit_px, pnl_net_usdt, commission, is_long]
    tr = json.load(open(ENGINE_JSON))["trades"]
    tr.sort(key=lambda t: t[1])
    assert len(tr) == 336
    return tr


def load_bars():
    out = {}
    with open(OHLCV_CSV) as f:
        for r in csv.DictReader(f):
            out[int(r["timestamp"])] = (float(r["open"]), float(r["high"]),
                                        float(r["low"]), float(r["close"]))
    return out


def gross_excursions(trade, bars):
    """Gross MFE/MAE in symbol ccy from entry fill to exit fill (exit bar
    contributes only its open — position closes at the exit-bar open)."""
    e_ms, x_ms, e_px, x_px, pnl, comm, is_long = trade
    dr = 1 if is_long else -1
    mfe = mae = 0.0
    for b in sorted(t for t in bars if e_ms <= t <= x_ms):
        o, h, l, c = bars[b]
        if b == x_ms:
            mfe = max(mfe, dr * (o - e_px))
            mae = max(mae, -dr * (o - e_px))
            break
        mfe = max(mfe, (h - e_px) if dr > 0 else (e_px - l))
        mae = max(mae, (e_px - l) if dr > 0 else (h - e_px))
    return mfe, mae


def fmt_ms(ms):
    return datetime.datetime.fromtimestamp(ms / 1000, UTC).strftime("%Y-%m-%d %H:%M")


# ------------------------------------------------------------------- sections
def section_sharpe(tr):
    print("== 1. Sharpe / Sortino (monthly, UTC, realized USD equity) ==")
    cum = 0.0
    month_end = {}                       # (y, m) -> realized USD equity
    for t in tr:
        cum += rate_prev_utc_day(t[1]) * t[4]
        d = datetime.datetime.fromtimestamp(t[1] / 1000, UTC)
        month_end[(d.year, d.month)] = INITIAL + cum
    eqs = [month_end[k] for k in sorted(month_end)]
    rets, prev = [], INITIAL
    for e in eqs:
        rets.append(e / prev - 1.0)
        prev = e
    n = len(rets)
    mean = sum(rets) / n
    sd_pop = math.sqrt(sum((r - mean) ** 2 for r in rets) / n)
    sd_smp = math.sqrt(sum((r - mean) ** 2 for r in rets) / (n - 1))
    dd_pop = math.sqrt(sum(min(0.0, r - RF_MONTHLY) ** 2 for r in rets) / n)
    sharpe = (mean - RF_MONTHLY) / sd_pop
    sortino = (mean - RF_MONTHLY) / dd_pop
    print(f"  n={n} monthly returns (UTC buckets, baseline 1,000,000)")
    print(f"  Sharpe  (population stddev, not annualized) = {sharpe:9.4f}   TV {TARGETS['sharpe']}")
    print(f"  Sharpe  (sample stddev, for reference)      = {(mean-RF_MONTHLY)/sd_smp:9.4f}   <- engine-style ddof=1")
    print(f"  Sortino (population downside vs rf)         = {sortino:9.4f}   TV {TARGETS['sortino']}")
    ok = abs(sharpe - TARGETS["sharpe"]) <= 0.01
    print(f"  -> Sharpe within +-0.01: {'YES' if ok else 'NO'}")
    return sharpe, sortino


def section_c2c(tr):
    print("\n== 2. Close-to-close run-up / drawdown (trade-close USD equity, global-extreme phases) ==")
    cum = [0.0]
    for t in tr:
        cum.append(cum[-1] + rate_prev_utc_day(t[1]) * t[4])
    ex_ms = [t[1] for t in tr]
    N = len(tr)
    gmax = max(range(1, N + 1), key=lambda i: cum[i])
    gmin = min(range(1, N + 1), key=lambda i: cum[i])
    assert gmax < gmin, "phase logic below assumes peak-before-trough shape"
    ru1, dd = cum[gmax] - cum[1], cum[gmax] - cum[gmin]
    ru2 = cum[N] - cum[gmin]
    d_ru1 = (ex_ms[gmax - 1] - ex_ms[0]) / 86400000
    d_dd = (ex_ms[gmin - 1] - ex_ms[gmax - 1]) / 86400000
    d_ru2 = (ex_ms[N - 1] - ex_ms[gmin - 1]) / 86400000
    print(f"  series: realized USD equity at the 336 trade exits; phases split at")
    print(f"  global max trade #{gmax} ({fmt_ms(ex_ms[gmax-1])}, {cum[gmax]:+.4f}) and")
    print(f"  global min trade #{gmin} ({fmt_ms(ex_ms[gmin-1])}, {cum[gmin]:+.4f})")
    print(f"  run-up #1 {ru1:9.4f}  ({d_ru1:6.2f} d)   start -> global max")
    print(f"  drawdown  {dd:9.4f}  ({d_dd:6.2f} d)   global max -> global min   TV {TARGETS['dd_c2c']}")
    print(f"  run-up #2 {ru2:9.4f}  ({d_ru2:6.2f} d)   global min -> end")
    print(f"  Max run-up   = {max(ru1, ru2):8.2f}   TV {TARGETS['ru_c2c']}")
    print(f"  Avg run-up   = {(ru1+ru2)/2:8.2f}   TV {TARGETS['ru_avg']}")
    print(f"  Avg ru dur   = floor({(d_ru1+d_ru2)/2:.2f}) = {int((d_ru1+d_ru2)/2)} days   TV {TARGETS['ru_dur_days']} days")
    print(f"  Max/Avg dd   = {dd:8.2f}   TV {TARGETS['dd_avg']}")
    print(f"  Avg dd dur   = floor({d_dd:.2f}) = {int(d_dd)} days   TV {TARGETS['dd_dur_days']} days")
    # classic definitions, for the delta table
    rm, rmin, cdd, cru = -1e18, 1e18, 0.0, 0.0
    for e in cum[1:]:
        rm, rmin = max(rm, e), min(rmin, e)
        cdd, cru = max(cdd, rm - e), max(cru, e - rmin)
    print(f"  [classic cummax dd = {cdd:.2f} (coincides); classic cummin run-up = {cru:.2f} != 39.20 -> phase rule is decisive]")
    return cum


def section_intrabar(tr, cum, bars):
    print("\n== 3. Intrabar run-up / drawdown (settled reference vs excursion events) ==")
    # per-trade excursion events in USD, netted of entry commission (TV's
    # Favorable/Adverse excursion convention, verified vs xlsx columns)
    dd = ru = 0.0
    dd_info = ru_info = None
    runmax_settled = 0.0          # settled = realized cums + entry-comm dips
    runmin_settled = 0.0
    rmin_at = ("initial", 0)
    for n, t in enumerate(tr, 1):
        r_exit = rate_prev_utc_day(t[1])
        comm_entry = COMM_RATE * t[2] * rate_prev_utc_day(t[0])
        entry_pt = cum[n - 1] - comm_entry
        if entry_pt < runmin_settled:
            runmin_settled, rmin_at = entry_pt, ("entry", n)
        mfe, mae = gross_excursions(t, bars)
        hi = cum[n - 1] + (mfe - COMM_RATE * t[2]) * r_exit
        lo = cum[n - 1] + (-mae - COMM_RATE * t[2]) * r_exit
        if runmax_settled - lo > dd:
            dd, dd_info = runmax_settled - lo, (n, runmax_settled, lo)
        if hi - runmin_settled > ru:
            ru, ru_info = hi - runmin_settled, (n, hi, runmin_settled, rmin_at)
        if cum[n] > runmax_settled:
            runmax_settled = cum[n]
        if cum[n] < runmin_settled:
            runmin_settled, rmin_at = cum[n], ("close", n)
    n, pk, lo = dd_info
    print(f"  Max drawdown (intrabar) = {dd:9.4f}   TV {TARGETS['dd_intra']}")
    print(f"    peak = settled {pk:+.4f}, trough = adverse excursion of trade #{n} ({fmt_ms(tr[n-1][1])})")
    n, hi, ref, at = ru_info
    print(f"  Max run-up (intrabar)   = {ru:9.4f}   TV {TARGETS['ru_intra']}")
    print(f"    peak = favorable excursion of trade #{n} ({fmt_ms(tr[n-1][1])}, {hi:+.2f}),")
    print(f"    trough = settled ref {ref:+.2f} = equity at {at[0]} of trade #{at[1]} (entry-commission dip)")
    print("  [naive full-envelope runmax/runmin give 2076-2077 / 292-304 -> the settled-reference rule is decisive]")
    return dd, ru


def section_cagr(tr):
    print("\n== 4. Annualized return (CAGR) ==")
    for name, sel, tv in [("all", lambda t: True, -0.18),
                          ("long", lambda t: t[6], -0.08),
                          ("short", lambda t: not t[6], -0.11)]:
        net = sum(rate_prev_utc_day(t[1]) * t[4] for t in tr if sel(t))
        cagr = ((INITIAL + net) / INITIAL) ** (365.0 / BACKTEST_DAYS) - 1
        print(f"  {name:5s}: net {net:9.2f} USD -> ({(INITIAL+net)/INITIAL:.8f})^(365/396) - 1 "
              f"= {cagr*100:8.4f}%   TV {tv}")
    print("  [engine -0.19% uses the traded span 393.75 d; TV uses the configured")
    print("   backtesting range = 396.0 d with a 365-day year]")


def section_xlsx_check(tr, bars):
    if not os.path.exists(XLSX):
        print("\n(xlsx not found; skipping reference cross-check)")
        return
    try:
        import openpyxl
    except ImportError:
        print("\n(openpyxl missing; skipping reference cross-check)")
        return
    print("\n== 5. Cross-check vs TV's own per-trade columns (xlsx) ==")
    wb = openpyxl.load_workbook(XLSX, data_only=True)
    ex = {r[0]: r for r in list(wb["Trades"].iter_rows(values_only=True))[1:]
          if r[1].startswith("Exit")}
    bad = 0
    for n, t in enumerate(tr, 1):
        mfe, mae = gross_excursions(t, bars)
        r = rate_prev_utc_day(t[1])
        pf = max(0.0, (mfe - COMM_RATE * t[2])) * r
        pa = -(mae + COMM_RATE * t[2]) * r
        if abs(pf - ex[n][9]) > 0.011 or abs(pa - ex[n][11]) > 0.011:
            bad += 1
    print(f"  excursion model (gross -/+ entry comm, exit-day rate): "
          f"{336-bad}/336 trades match TV's FE/AE columns within 1 cent")
    # exact-from-TV-columns recomputation of the two intrabar metrics
    cumx = [0.0] + [ex[n][13] for n in range(1, 337)]
    rm = rmin = dd = ru = 0.0
    for n in range(1, 337):
        entry_pt = cumx[n - 1] - COMM_RATE * tr[n - 1][2]
        rmin = min(rmin, entry_pt)
        hi, lo = cumx[n - 1] + ex[n][9], cumx[n - 1] + ex[n][11]
        dd = max(dd, rm - lo)
        ru = max(ru, hi - rmin)
        rm, rmin = max(rm, cumx[n]), min(rmin, cumx[n])
    print(f"  same formulas on TV's own columns: dd {dd:.4f} (TV 2072.41), ru {ru:.4f} (TV 285.65)")


def delta_table():
    print("""
== Definition delta table (TV equity panel vs engine) ==
  metric                     TV convention (cracked)                                  engine today
  -------------------------  -------------------------------------------------------  -------------------------------------
  Sharpe -13.787             monthly simple returns of month-end REALIZED equity in    sample stddev, NY-tz months, USDT
                             USD, UTC month buckets, rf 2%/12, POPULATION stddev,      marked equity -> -13.5932
                             not annualized -> -13.7873
  Sortino -0.997             same series, population downside dev vs rf -> -0.9974     matches (-0.9975)
  Max dd (c2c) 2068.23       trade-close USD equity, global-max -> global-min phase    per-bar USDT cummax dd 2071.01;
                             (== classic cummax dd on that series) -> 2068.24*         daily-sampled 2060.63
  Avg dd / 374 days          one phase: value 2068.23, floor(374.55 d)                 n/a
  Max run-up (c2c) 39.20     phase global-min -> series end = 39.1959                  cummin run-up 260+ (different def)
  Avg run-up 29.48 / 9 d     mean of the 2 run-up phases (19.76, 39.20); floor(9.32)   n/a
  Max dd (intrabar) 2072.41  runmax(settled) - per-trade adverse excursion event;      n/a (engine has per-trade MAE)
                             settled = realized + entry-commission dips -> 2072.41
  Max run-up (intra) 285.65  per-trade favorable excursion event - runmin(settled)     n/a (engine has per-trade MFE)
                             -> 285.65
  CAGR -0.18%                (1+ret)^(365/396) - 1, D = configured backtest range      -0.19% (traded span 393.75 d)
  * 0.01 residuals are USDTUSD rate-source noise (Coinbase proxy vs TV's feed),
    same one-cent class as the known trade #261 discrepancy.""")


def main():
    tr = load_trades()
    bars = load_bars()
    section_sharpe(tr)
    cum = section_c2c(tr)
    section_intrabar(tr, cum, bars)
    section_cagr(tr)
    section_xlsx_check(tr, bars)
    delta_table()


if __name__ == "__main__":
    sys.exit(main())
