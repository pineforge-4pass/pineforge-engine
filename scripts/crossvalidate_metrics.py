#!/usr/bin/env python3
"""Cross-validate pineforge equity/risk metrics against quantstats and
empyrical-reloaded.

OPTIONAL OFFLINE VALIDATION TOOL — not part of the build or the ctest
suite. The engine repo takes no new Python dependencies; run this from a
throwaway venv OUTSIDE the repo:

    python3 -m venv /tmp/pf-xval-venv
    /tmp/pf-xval-venv/bin/pip install quantstats empyrical-reloaded pandas numpy
    /tmp/pf-xval-venv/bin/python scripts/crossvalidate_metrics.py \\
        corpus/validation/composite-4emarsi-integration-01

What it does
------------
Loads a corpus strategy library through scripts/run_strategy.py's ctypes
ABI mirrors, runs the backtest while keeping the pf_report_t alive long
enough to read `metrics.equity` AND the raw per-bar `equity_curve`, then
recomputes the equity/risk statistics three independent ways:

  1. plain numpy, replicating the engine's documented conventions
     (include/pineforge/pineforge.h doxygen is authoritative);
  2. empyrical-reloaded, with explicit convention adapters;
  3. quantstats.stats, with explicit convention adapters.

Engine conventions being validated (see pf_equity_stats_t doxygen):
  - per-bar simple returns r_i = E_i/E_{i-1}-1 over the equity curve
    (E = initial_capital + net_profit + open_profit at bar close,
    indexed by script-bar OPEN time in ms);
  - bars_per_year = (n-1)/span_years,
    span_years = (t_last-t_first)/(365.25*86400e3);
  - sharpe_bar  = (mean(r)-rf_bar)/sample_sd(r, N-1)*sqrt(bpy),
    rf_bar = 0.02/bpy;
  - sortino_bar = (mean(r)-rf_bar)/pop_downside_dev(r vs rf_bar)*sqrt(bpy);
  - sharpe_tv/sortino_tv: same construction over month-end-resampled
    equities (UTC calendar-month bucketing of bar-open times when the
    chart tz is empty), rf = 0.02/12, annualized by sqrt(12);
  - max_equity_drawdown: peak-to-trough walk over the curve (USD), pct
    taken vs the peak in effect WHEN THE MAX-USD DRAWDOWN was hit (which
    can differ from the maximum fractional drawdown the libraries report);
  - cagr = 100*((E_final/initial_capital)^(1/span_years)-1) — CALENDAR
    span, base = declared initial_capital (not first curve equity);
  - calmar = cagr_pct / max_dd_pct;  recovery = net_profit / max_dd_usd.

Library convention adapters applied (verified against installed sources):
  - empyrical.sharpe_ratio(r, risk_free=p, annualization=N)
      = mean(r-p)/std(r-p, ddof=1)*sqrt(N)        -> engine-equivalent
        when p = rf-per-period and N = bpy (constant shift leaves sd).
  - empyrical.sortino_ratio(..., required_return=p, annualization=N)
      = mean(r-p)*N / (sqrt(mean(clip(r-p,max=0)^2))*sqrt(N))
      = mean(r-p)/pop_dd*sqrt(N)                  -> engine-equivalent.
  - empyrical.max_drawdown(returns) = most-negative fractional drawdown
    (peak-to-trough on the compounded curve), NOT pct-at-max-USD.
  - empyrical.annual_return(r, annualization=bpy):
      (prod(1+r))^(bpy/len(r))-1. Because bpy = (n-1)/span_years and
      len(r) = n-1, the exponent equals 1/span_years — i.e. the
      periods-based formula coincides with the calendar formula here,
      EXCEPT its base is the first curve equity E_0, not initial_capital.
  - quantstats.stats.sharpe/sortino(r, rf=A, periods=N) de-annualize the
    ANNUAL rf GEOMETRICALLY: rf_p = (1+A)^(1/N)-1, vs the engine's
    arithmetic A/N. Two variants are reported: "adapted" feeds
    pre-excess returns (r - rf_per_period) with rf=0 (must match the
    engine exactly), and "native rf" passes rf=0.02 (tiny expected
    geometric-deannualization delta).
  - quantstats.stats.max_drawdown compounds returns to prices with a
    phantom pre-start baseline -> max fractional drawdown incl. E_0.

Output: one comparison table per metric family with relative deltas vs
the engine value. |rel| <= 1e-6 -> "match"; 1e-6 < |rel| <= 1e-3 ->
"CONVENTION-DELTA"; |rel| > 1e-3 -> "MISMATCH". Rows whose library
convention is KNOWN to differ are labelled with the differing convention
so a flagged delta there is expected, not an engine bug. Exit code is 1
if any unexplained MISMATCH row (an engine-convention row, not a
known-different-convention row) fails.

Usage
-----
    crossvalidate_metrics.py STRATEGY_DIR [--trim-end-ms MS] [--ohlcv CSV]
"""
from __future__ import annotations

import argparse
import ctypes
import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_strategy as rs  # noqa: E402  (ABI mirrors + Strategy loader)

import numpy as np          # noqa: E402
import pandas as pd         # noqa: E402
import empyrical as ep      # noqa: E402
import quantstats as qs     # noqa: E402

RF_ANNUAL = 0.02
MS_PER_YEAR = 365.25 * 86400.0 * 1000.0


# --------------------------------------------------------------------------
# Engine run: replicate the minimal flow of run_strategy.Strategy.run but
# keep the report alive so metrics.equity + equity_curve can be read
# before report_free.
# --------------------------------------------------------------------------

def run_engine(strategy_dir: Path, ohlcv: Path, trim_end_ms: int | None) -> dict:
    so_path = strategy_dir / "strategy.so"
    if not so_path.exists():
        for alt in ("strategy.dylib", "strategy.so", "strategy.dll"):
            cand = strategy_dir / alt
            if cand.exists():
                so_path = cand
                break
    strat = rs.Strategy(so_path)   # loads lib, asserts pf_abi_version
    lib = strat.lib

    params: dict = {}
    inputs_path = strategy_dir / "inputs.json"
    if inputs_path.exists():
        with inputs_path.open(encoding="utf-8") as f:
            params = json.load(f)

    ohlcv_path = ohlcv
    if isinstance(params, dict) and "ohlcv_csv" in params:
        v = str(params["ohlcv_csv"])
        ohlcv_path = (Path(v) if v.startswith("/") else strategy_dir / v).resolve()
    start_ms = None
    if isinstance(params, dict) and "ohlcv_start_ms" in params:
        try:
            start_ms = int(params["ohlcv_start_ms"])
        except (TypeError, ValueError):
            start_ms = None

    bars, n = rs._load_bars(ohlcv_path, ohlcv_start_ms=start_ms)
    if trim_end_ms is not None:
        kept = [i for i in range(n) if bars[i].timestamp <= trim_end_ms]
        trimmed = (rs.BarC * len(kept))()
        for j, i in enumerate(kept):
            trimmed[j] = bars[i]
        bars, n = trimmed, len(kept)
    if n < 3:
        raise SystemExit(f"too few bars after trim: {n}")

    chart_tz = str(params.get("chart_timezone") or "") if isinstance(params, dict) else ""

    state = lib.strategy_create(json.dumps(params).encode())
    report = rs.ReportC()
    try:
        if chart_tz and hasattr(lib, "strategy_set_chart_timezone"):
            lib.strategy_set_chart_timezone(state, chart_tz.encode())
        if hasattr(lib, "strategy_set_input") and isinstance(params, dict):
            for key, value in params.items():
                if key.startswith("tv_") or key in rs._VALIDATION_META_KEYS:
                    continue
                lib.strategy_set_input(state, str(key).encode(), str(value).encode())
        lib.run_backtest_full(state, bars, n, b"", b"", 0, 4, 3,
                              ctypes.byref(report))
        if hasattr(lib, "strategy_get_last_error"):
            err = lib.strategy_get_last_error(state)
            if err:
                msg = err.decode("utf-8", "replace")
                if msg:
                    raise RuntimeError("pineforge engine rejected run: " + msg)

        eq = report.metrics.equity
        engine = {f: float(getattr(eq, f)) for f, _ in eq._fields_}
        m = int(report.equity_curve_len)
        t_ms = np.empty(m, dtype=np.int64)
        equity = np.empty(m, dtype=np.float64)
        open_pl = np.empty(m, dtype=np.float64)
        for i in range(m):
            p = report.equity_curve[i]
            t_ms[i] = p.time_ms
            equity[i] = p.equity
            open_pl[i] = p.open_profit
        return {
            "engine": engine,
            "net_profit": float(report.net_profit),
            "total_trades": int(report.total_trades),
            "t_ms": t_ms,
            "equity": equity,
            "open_pl": open_pl,
            "chart_tz": chart_tz,
        }
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)


# --------------------------------------------------------------------------
# numpy third opinion — engine conventions reimplemented from the
# pineforge.h doxygen, independent of src/engine_metrics.cpp.
# --------------------------------------------------------------------------

def np_drawdown(equity: np.ndarray) -> dict:
    peak = equity[0]
    max_dd_usd = 0.0
    pct_at_max_usd = 0.0
    max_dd_frac = 0.0          # library convention: max fractional dd
    for eq in equity:
        if eq > peak:
            peak = eq
        dd = peak - eq
        if dd > max_dd_usd:
            max_dd_usd = dd
            pct_at_max_usd = dd / peak * 100.0 if peak > 0 else float("nan")
        if peak > 0 and dd / peak > max_dd_frac:
            max_dd_frac = dd / peak
    return {"usd": max_dd_usd, "pct_at_max_usd": pct_at_max_usd,
            "max_frac": max_dd_frac}


def np_sharpe_sortino(r: np.ndarray, rf_period: float, ann: float) -> tuple[float, float]:
    n = len(r)
    if n < 2:
        return float("nan"), float("nan")
    mean = r.mean()
    sd = r.std(ddof=1)
    down = np.minimum(0.0, r - rf_period)
    dd = math.sqrt(float((down * down).sum()) / n)
    sharpe = (mean - rf_period) / sd * ann if sd > 0 else float("nan")
    sortino = (mean - rf_period) / dd * ann if dd > 0 else float("nan")
    return float(sharpe), float(sortino)


def month_end_equities_utc(t_ms: np.ndarray, equity: np.ndarray) -> pd.Series:
    """Last equity of each UTC calendar month, bucketed by bar OPEN time.
    Mirrors the engine's month_key_utc walk (empty chart tz => UTC)."""
    idx = pd.to_datetime(t_ms, unit="ms", utc=True)
    s = pd.Series(equity, index=idx)
    return s.resample("ME").last().dropna()


# --------------------------------------------------------------------------
# Comparison / reporting
# --------------------------------------------------------------------------

class Table:
    def __init__(self, title: str):
        self.title = title
        self.rows: list[tuple[str, float, float | None, str, bool]] = []
        self.failures: list[str] = []

    def add(self, label: str, engine: float, other: float | None,
            known_convention_delta: bool = False):
        """known_convention_delta: row compares against a library value whose
        convention is documented to differ; a flag there is expected."""
        self.rows.append((label, engine, other, "", known_convention_delta))

    def render(self) -> str:
        out = [f"\n  {self.title}", "  " + "-" * 100]
        out.append(f"  {'field / source':56s} {'engine':>14s} {'other':>14s} "
                   f"{'rel.delta':>11s}  verdict")
        for label, eng, other, _, known in self.rows:
            if other is None or (isinstance(other, float) and math.isnan(other)):
                out.append(f"  {label:56s} {eng:14.8f} {'n/a':>14s} {'':>11s}  -")
                continue
            denom = max(1e-12, abs(eng))
            rel = (other - eng) / denom
            if abs(rel) <= 1e-6:
                verdict = "match"
            elif abs(rel) <= 1e-3:
                verdict = "CONVENTION-DELTA"
            else:
                verdict = "MISMATCH"
            if known and verdict != "match":
                verdict += " (expected: differing convention)"
            elif verdict == "MISMATCH":
                self.failures.append(label)
            out.append(f"  {label:56s} {eng:14.8f} {other:14.8f} "
                       f"{rel:11.2e}  {verdict}")
        return "\n".join(out)


def crossvalidate(strategy_dir: Path, ohlcv: Path, trim_end_ms: int | None,
                  initial_capital: float | None) -> int:
    run = run_engine(strategy_dir, ohlcv, trim_end_ms)
    eng = run["engine"]
    t_ms, equity = run["t_ms"], run["equity"]
    n = len(equity)
    net_profit = run["net_profit"]

    # initial_capital is not in pf_report_t; default 1e6 matches the corpus
    # strategies' strategy() declarations (override with --initial-capital).
    cap = initial_capital if initial_capital is not None else 1_000_000.0

    span_years = float(t_ms[-1] - t_ms[0]) / MS_PER_YEAR
    bpy = (n - 1) / span_years
    rf_bar = RF_ANNUAL / bpy
    rf_month = RF_ANNUAL / 12.0

    assert (equity > 0).all(), "curve has non-positive equity; adapters assume E>0"
    r = equity[1:] / equity[:-1] - 1.0
    idx = pd.to_datetime(t_ms[1:], unit="ms", utc=True)
    r_s = pd.Series(r, index=idx)

    me = month_end_equities_utc(t_ms, equity)
    mr = me.to_numpy()
    m_ret = mr[1:] / mr[:-1] - 1.0
    m_ret_s = pd.Series(m_ret, index=me.index[1:])

    print(f"\n=== {strategy_dir.name} ===")
    print(f"  bars(curve)={n}  trades={run['total_trades']}  "
          f"net_profit={net_profit:.2f}  span_years={span_years:.4f}  "
          f"bars_per_year={bpy:.2f}  months={len(mr)}  "
          f"chart_tz={'UTC' if not run['chart_tz'] else run['chart_tz']}")
    print(f"  curve: {pd.Timestamp(t_ms[0], unit='ms', tz='UTC')} .. "
          f"{pd.Timestamp(t_ms[-1], unit='ms', tz='UTC')}  "
          f"E0={equity[0]:.2f}  E_final={equity[-1]:.2f}  cap={cap:.0f}")

    tables: list[Table] = []

    # --- drawdown ---------------------------------------------------------
    dd = np_drawdown(equity)
    t = Table("max_equity_drawdown")
    t.add("max_dd USD          | numpy engine-walk", eng["max_equity_drawdown"], dd["usd"])
    t.add("max_dd_pct          | numpy pct@max-USD (engine conv.)",
          eng["max_equity_drawdown_pct"], dd["pct_at_max_usd"])
    t.add("max_dd_pct          | numpy max fractional dd",
          eng["max_equity_drawdown_pct"], dd["max_frac"] * 100.0,
          known_convention_delta=True)
    t.add("max_dd_pct          | empyrical.max_drawdown (max frac)",
          eng["max_equity_drawdown_pct"], -ep.max_drawdown(r_s) * 100.0,
          known_convention_delta=True)
    t.add("max_dd_pct          | quantstats.max_drawdown (max frac)",
          eng["max_equity_drawdown_pct"], -qs.stats.max_drawdown(r_s) * 100.0,
          known_convention_delta=True)
    tables.append(t)
    # cross-check the two library values against the numpy max-frac walk
    lib_dd_ok = (abs(-ep.max_drawdown(r_s) - dd["max_frac"]) <= 1e-9 * max(1.0, dd["max_frac"]),
                 abs(-qs.stats.max_drawdown(r_s) - dd["max_frac"]) <= 1e-9 * max(1.0, dd["max_frac"]))

    # --- per-bar sharpe / sortino ------------------------------------------
    np_sh, np_so = np_sharpe_sortino(r, rf_bar, math.sqrt(bpy))
    t = Table(f"sharpe_bar / sortino_bar   (bpy={bpy:.4f}, rf/bar={rf_bar:.3e})")
    t.add("sharpe_bar          | numpy engine conv.", eng["sharpe_bar"], np_sh)
    t.add("sharpe_bar          | empyrical(risk_free=rf_bar, ann=bpy)",
          eng["sharpe_bar"], ep.sharpe_ratio(r_s, risk_free=rf_bar, annualization=bpy))
    t.add("sharpe_bar          | quantstats adapted (r-rf_bar, rf=0)",
          eng["sharpe_bar"], qs.stats.sharpe(r_s - rf_bar, rf=0.0, periods=bpy))
    t.add("sharpe_bar          | quantstats native rf (geometric deann.)",
          eng["sharpe_bar"], qs.stats.sharpe(r_s, rf=RF_ANNUAL, periods=bpy),
          known_convention_delta=True)
    t.add("sortino_bar         | numpy engine conv.", eng["sortino_bar"], np_so)
    t.add("sortino_bar         | empyrical(required_return=rf_bar, ann=bpy)",
          eng["sortino_bar"], ep.sortino_ratio(r_s, required_return=rf_bar, annualization=bpy))
    t.add("sortino_bar         | quantstats adapted (r-rf_bar, rf=0)",
          eng["sortino_bar"], qs.stats.sortino(r_s - rf_bar, rf=0.0, periods=bpy))
    t.add("sortino_bar         | quantstats native rf (geometric deann.)",
          eng["sortino_bar"], qs.stats.sortino(r_s, rf=RF_ANNUAL, periods=bpy),
          known_convention_delta=True)
    tables.append(t)

    # --- TV monthly sharpe / sortino ----------------------------------------
    np_sh_tv, np_so_tv = np_sharpe_sortino(m_ret, rf_month, math.sqrt(12.0))
    t = Table(f"sharpe_tv / sortino_tv   (monthly, {len(m_ret)} returns, rf/mo={rf_month:.6f})")
    t.add("sharpe_tv           | numpy engine conv.", eng["sharpe_tv"], np_sh_tv)
    t.add("sharpe_tv           | empyrical(risk_free=rf/12, ann=12)",
          eng["sharpe_tv"], ep.sharpe_ratio(m_ret_s, risk_free=rf_month, annualization=12))
    t.add("sharpe_tv           | quantstats adapted (m-rf/12, rf=0)",
          eng["sharpe_tv"], qs.stats.sharpe(m_ret_s - rf_month, rf=0.0, periods=12))
    t.add("sharpe_tv           | quantstats native rf (geometric deann.)",
          eng["sharpe_tv"], qs.stats.sharpe(m_ret_s, rf=RF_ANNUAL, periods=12),
          known_convention_delta=True)
    t.add("sortino_tv          | numpy engine conv.", eng["sortino_tv"], np_so_tv)
    t.add("sortino_tv          | empyrical(required_return=rf/12, ann=12)",
          eng["sortino_tv"], ep.sortino_ratio(m_ret_s, required_return=rf_month, annualization=12))
    t.add("sortino_tv          | quantstats adapted (m-rf/12, rf=0)",
          eng["sortino_tv"], qs.stats.sortino(m_ret_s - rf_month, rf=0.0, periods=12))
    t.add("sortino_tv          | quantstats native rf (geometric deann.)",
          eng["sortino_tv"], qs.stats.sortino(m_ret_s, rf=RF_ANNUAL, periods=12),
          known_convention_delta=True)
    tables.append(t)

    # --- CAGR ----------------------------------------------------------------
    np_cagr_cal = 100.0 * ((equity[-1] / cap) ** (1.0 / span_years) - 1.0)
    np_cagr_e0 = 100.0 * ((equity[-1] / equity[0]) ** (1.0 / span_years) - 1.0)
    t = Table("cagr (engine: calendar span, base = initial_capital)")
    t.add("cagr                | numpy calendar, base=cap", eng["cagr"], np_cagr_cal)
    t.add("cagr                | numpy calendar, base=E0", eng["cagr"], np_cagr_e0,
          known_convention_delta=True)
    # empyrical annual_return(annualization=bpy): exponent bpy/len(r) ==
    # 1/span_years exactly (bpy = (n-1)/span_years, len(r) = n-1), so the
    # ONLY remaining difference vs the engine is the base (E0 vs cap).
    t.add("cagr                | empyrical.annual_return(ann=bpy) [base=E0]",
          eng["cagr"], 100.0 * ep.annual_return(r_s, annualization=bpy),
          known_convention_delta=True)
    t.add("cagr                | quantstats.cagr(periods=bpy) [base=E0]",
          eng["cagr"], 100.0 * qs.stats.cagr(r_s, periods=bpy),
          known_convention_delta=True)
    tables.append(t)
    # prove the periods-based==calendar equivalence on this data:
    ep_ann = float(ep.annual_return(r_s, annualization=bpy))
    eq0_delta = abs(100.0 * ep_ann - np_cagr_e0)

    # --- calmar / recovery -----------------------------------------------------
    np_calmar = (np_cagr_cal / dd["pct_at_max_usd"]
                 if dd["pct_at_max_usd"] > 0 else float("nan"))
    np_recovery = net_profit / dd["usd"] if dd["usd"] > 0 else float("nan")
    t = Table("calmar / recovery_factor")
    t.add("calmar              | numpy engine conv. (cagr%/dd%@maxUSD)",
          eng["calmar"], np_calmar)
    ep_calmar = float(ep.calmar_ratio(r_s, annualization=bpy))
    t.add("calmar              | empyrical.calmar_ratio [base=E0, max-frac dd]",
          eng["calmar"], ep_calmar, known_convention_delta=True)
    qs_calmar = float(qs.stats.calmar(r_s, periods=bpy))
    t.add("calmar              | quantstats.calmar [base=E0, max-frac dd]",
          eng["calmar"], qs_calmar, known_convention_delta=True)
    t.add("recovery_factor     | numpy engine conv. (net_profit/ddUSD)",
          eng["recovery_factor"], np_recovery)
    # quantstats.recovery_factor = |sum(r)| / |max_dd_frac| — ARITHMETIC sum
    # of returns over fractional dd; reproduce it by hand to prove the
    # library value, then show it next to the engine's USD-based ratio.
    qs_rec = float(qs.stats.recovery_factor(r_s))
    qs_rec_hand = abs(float(r.sum())) / dd["max_frac"] if dd["max_frac"] > 0 else float("nan")
    t.add("recovery_factor     | quantstats native (sum(r)/frac-dd)",
          eng["recovery_factor"], qs_rec, known_convention_delta=True)
    tables.append(t)

    for tb in tables:
        print(tb.render())

    print("\n  cross-checks:")
    print(f"    empyrical max_drawdown == numpy max-frac walk: {lib_dd_ok[0]}")
    print(f"    quantstats max_drawdown == numpy max-frac walk: {lib_dd_ok[1]}")
    # Prove the quantstats "native rf" rows are FULLY explained by its
    # geometric rf de-annualization rf_p=(1+A)^(1/N)-1 (vs engine A/N):
    # recompute quantstats' own formula by hand with the geometric rf.
    for label, series, periods in (("bar", r, bpy), ("tv", m_ret, 12.0)):
        rf_geo = (1.0 + RF_ANNUAL) ** (1.0 / periods) - 1.0
        ex = series - rf_geo
        hand_sh = ex.mean() / ex.std(ddof=1) * math.sqrt(periods)
        down = ex[ex < 0]
        ddev = math.sqrt(float((down * down).sum()) / len(ex))
        hand_so = ex.mean() / ddev * math.sqrt(periods) if ddev > 0 else float("nan")
        qs_sh = float(qs.stats.sharpe(pd.Series(series, index=(idx if label == "bar" else me.index[1:])),
                                      rf=RF_ANNUAL, periods=periods))
        qs_so = float(qs.stats.sortino(pd.Series(series, index=(idx if label == "bar" else me.index[1:])),
                                       rf=RF_ANNUAL, periods=periods))
        print(f"    qs native sharpe_{label} hand-recompute (geometric rf): "
              f"|delta|={abs(qs_sh - hand_sh):.3e}")
        print(f"    qs native sortino_{label} hand-recompute (geometric rf): "
              f"|delta|={abs(qs_so - hand_so):.3e}")
    print(f"    empyrical.annual_return(ann=bpy) == calendar formula on base E0: "
          f"|delta|={eq0_delta:.3e} pct-points")
    if not math.isnan(qs_rec_hand):
        print(f"    quantstats.recovery_factor hand-recompute |delta|="
              f"{abs(qs_rec - qs_rec_hand):.3e}")

    failures = [f"{tb.title}: {lbl}" for tb in tables for lbl in tb.failures]
    if failures:
        print("\n  UNEXPLAINED MISMATCHES (potential engine bug):")
        for f in failures:
            print(f"    - {f}")
        return 1
    print("\n  RESULT: all engine-convention rows match; every flagged row is a "
          "documented library-convention difference.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("strategy_dir", type=Path,
                    help="corpus/<cat>/<name>/ containing strategy.so|.dylib")
    ap.add_argument("--trim-end-ms", type=int, default=None,
                    help="Drop input bars with timestamp > this (Unix ms) "
                         "before the run, to test alternate spans.")
    ap.add_argument("--ohlcv", type=Path, default=rs.DEFAULT_OHLCV,
                    help="OHLCV CSV (default: the corpus default feed)")
    ap.add_argument("--initial-capital", type=float, default=None,
                    help="strategy() initial_capital (default 1e6, the corpus "
                         "convention; pf_report_t does not expose it)")
    args = ap.parse_args()
    return crossvalidate(args.strategy_dir.resolve(), args.ohlcv.resolve(),
                         args.trim_end_ms, args.initial_capital)


if __name__ == "__main__":
    sys.exit(main())
