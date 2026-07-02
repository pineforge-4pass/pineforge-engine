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

Corpus-wide mode (``--all``)
----------------------------
``--all`` sweeps every directory under corpus/validation/ that contains a
compiled strategy library, honouring each probe's ``inputs.json`` exactly
the way scripts/run_strategy.py main() does (ohlcv_csv / input_tf /
script_tf / ohlcv_start_ms / chart_timezone / runtime_overrides / input
params), runs the FULL feed (the TV comparison window is irrelevant here —
the comparison is engine curve vs libraries), and applies only the
ENGINE-CONVENTION adapters per strategy (numpy reimplementation,
empyrical with engine-equivalent arguments, quantstats with pre-excess
returns). Degenerate values are handled as explicit NaN-AGREEMENT checks,
not errors: a field where the engine reports NaN (flat curve, < 2 monthly
returns, zero drawdown) passes iff the reference is also non-finite, and
MISMATCHes if exactly one side is finite. Strategies whose engine run
fails are recorded as SKIP with the error string. Exit code 1 on any
MISMATCH.

Usage
-----
    crossvalidate_metrics.py STRATEGY_DIR [--trim-end-ms MS] [--ohlcv CSV]
    crossvalidate_metrics.py --all [--corpus-root DIR] [--ohlcv CSV]
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
import warnings
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
# Engine run: drive run_strategy.Strategy.run (which honours every
# inputs.json knob exactly like the corpus harness) and capture
# metrics.equity + the raw equity_curve through the on_report hook,
# which fires before report_free.
# --------------------------------------------------------------------------

_CAP_RE = re.compile(r"initial_capital_\s*=\s*([0-9][0-9eE+.\-]*)\s*;")


def declared_initial_capital(strategy_dir: Path, default: float = 1_000_000.0) -> float:
    """strategy() initial_capital from the codegen output (pf_report_t does
    not expose it). Falls back to the corpus convention of 1e6."""
    gen = strategy_dir / "generated.cpp"
    if gen.exists():
        m = _CAP_RE.search(gen.read_text(encoding="utf-8", errors="replace"))
        if m:
            try:
                return float(m.group(1))
            except ValueError:
                pass
    return default


def run_engine(strategy_dir: Path, ohlcv: Path, trim_end_ms: int | None) -> dict:
    so_path = rs.find_strategy_lib(strategy_dir)
    strat = rs.Strategy(so_path)   # loads lib, asserts pf_abi_version

    params: dict = {}
    inputs_path = strategy_dir / "inputs.json"
    if inputs_path.exists():
        with inputs_path.open(encoding="utf-8") as f:
            params = json.load(f)
    if not isinstance(params, dict):
        params = {}

    ohlcv_path, run_kwargs = rs.inputs_run_kwargs(params, strategy_dir, ohlcv)

    captured: dict = {}

    def _grab(report) -> None:
        eq = report.metrics.equity
        captured["engine"] = {f: float(getattr(eq, f)) for f, _ in eq._fields_}
        m = int(report.equity_curve_len)
        t_ms = np.empty(m, dtype=np.int64)
        equity = np.empty(m, dtype=np.float64)
        open_pl = np.empty(m, dtype=np.float64)
        for i in range(m):
            p = report.equity_curve[i]
            t_ms[i] = p.time_ms
            equity[i] = p.equity
            open_pl[i] = p.open_profit
        captured["t_ms"], captured["equity"], captured["open_pl"] = t_ms, equity, open_pl
        captured["net_profit"] = float(report.net_profit)
        captured["total_trades"] = int(report.total_trades)

    strat.run(ohlcv_path, params=params, ohlcv_end_ms=trim_end_ms,
              on_report=_grab, **run_kwargs)
    captured["chart_tz"] = run_kwargs["chart_timezone"]
    return captured


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


def month_end_equities(t_ms: np.ndarray, equity: np.ndarray,
                       chart_tz: str = "") -> pd.Series:
    """Last equity of each calendar month, bucketed by bar OPEN time.
    Mirrors the engine's month-key walk: empty chart tz => UTC, else the
    chart's IANA wall clock decides the month boundaries."""
    idx = pd.to_datetime(t_ms, unit="ms", utc=True)
    if chart_tz:
        idx = idx.tz_convert(chart_tz)
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
    if n < 3:
        raise SystemExit(f"curve too short for comparison: {n} points")
    net_profit = run["net_profit"]

    # initial_capital is not in pf_report_t; parsed from generated.cpp
    # (corpus default 1e6; override with --initial-capital).
    cap = (initial_capital if initial_capital is not None
           else declared_initial_capital(strategy_dir))

    span_years = float(t_ms[-1] - t_ms[0]) / MS_PER_YEAR
    bpy = (n - 1) / span_years
    rf_bar = RF_ANNUAL / bpy
    rf_month = RF_ANNUAL / 12.0

    assert (equity > 0).all(), "curve has non-positive equity; adapters assume E>0"
    r = equity[1:] / equity[:-1] - 1.0
    idx = pd.to_datetime(t_ms[1:], unit="ms", utc=True)
    r_s = pd.Series(r, index=idx)

    me = month_end_equities(t_ms, equity, run["chart_tz"])
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


# --------------------------------------------------------------------------
# Corpus-wide mode (--all): engine-convention adapters only, with explicit
# NaN-agreement semantics for degenerate strategies.
# --------------------------------------------------------------------------

def _lib_value(fn, *args, **kwargs):
    """Call a library statistic, silencing degenerate-data warnings.
    Returns float, or the Exception when the library refuses the input
    (treated as 'library says degenerate' by classify())."""
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with np.errstate(all="ignore"):
                return float(fn(*args, **kwargs))
    except Exception as exc:  # noqa: BLE001 — library-internal failure modes vary
        return exc


def classify(engine_val: float, other) -> tuple[str, float | None]:
    """NaN-aware verdict for one engine-convention comparison.

    - engine non-finite AND reference non-finite/refused -> 'nan-agree'
      (degenerate case: flat curve, < 2 monthly returns, zero drawdown);
    - exactly one side finite -> 'MISMATCH' (rel=None);
    - both finite -> rel-delta thresholds (1e-6 match / 1e-3 convention).
    """
    eng_degen = not math.isfinite(engine_val)
    oth_degen = isinstance(other, Exception) or other is None \
        or not math.isfinite(other)
    if eng_degen and oth_degen:
        return "nan-agree", None
    if eng_degen or oth_degen:
        return "MISMATCH", None
    rel = (other - engine_val) / max(1e-12, abs(engine_val))
    if abs(rel) <= 1e-6:
        return "match", rel
    if abs(rel) <= 1e-3:
        return "CONVENTION-DELTA", rel
    return "MISMATCH", rel


def engine_convention_checks(run: dict, cap: float) -> tuple[list, dict]:
    """Compute the engine-convention reference values for one strategy.

    Returns (checks, ctx) where checks is a list of
    (field, source, engine_value, reference_value_or_Exception). Only
    adapters that are engine-EQUIVALENT by construction are included
    (numpy reimplementation; empyrical with rf-per-period + ann=bpy;
    quantstats fed pre-excess returns with rf=0). Known-different library
    conventions (max fractional dd, base-E0 cagr, geometric rf, ...) are
    deliberately excluded here — they are covered, labelled, by the
    verbose single-strategy mode."""
    eng = run["engine"]
    t_ms, equity = run["t_ms"], run["equity"]
    n = len(equity)
    net_profit = run["net_profit"]

    span_years = float(t_ms[-1] - t_ms[0]) / MS_PER_YEAR
    bpy = (n - 1) / span_years
    rf_bar = RF_ANNUAL / bpy
    rf_month = RF_ANNUAL / 12.0

    r = equity[1:] / equity[:-1] - 1.0
    r_s = pd.Series(r, index=pd.to_datetime(t_ms[1:], unit="ms", utc=True))

    me = month_end_equities(t_ms, equity, run["chart_tz"])
    mr = me.to_numpy()
    if len(mr) >= 2:
        m_ret = mr[1:] / mr[:-1] - 1.0
        m_ret_s = pd.Series(m_ret, index=me.index[1:])
    else:
        m_ret = np.empty(0, dtype=np.float64)
        m_ret_s = pd.Series(m_ret, index=pd.DatetimeIndex([], tz="UTC"))

    dd = np_drawdown(equity)
    np_sh, np_so = np_sharpe_sortino(r, rf_bar, math.sqrt(bpy))
    np_sh_tv, np_so_tv = np_sharpe_sortino(m_ret, rf_month, math.sqrt(12.0))
    np_cagr = (100.0 * ((equity[-1] / cap) ** (1.0 / span_years) - 1.0)
               if span_years > 0 and equity[-1] > 0 and cap > 0 else float("nan"))
    np_calmar = (np_cagr / dd["pct_at_max_usd"]
                 if dd["pct_at_max_usd"] > 0 else float("nan"))
    np_recovery = net_profit / dd["usd"] if dd["usd"] > 0 else float("nan")

    checks = [
        ("max_dd_usd", "numpy", eng["max_equity_drawdown"], dd["usd"]),
        ("max_dd_pct", "numpy", eng["max_equity_drawdown_pct"], dd["pct_at_max_usd"]),
        ("sharpe_bar", "numpy", eng["sharpe_bar"], np_sh),
        ("sharpe_bar", "empyrical", eng["sharpe_bar"],
         _lib_value(ep.sharpe_ratio, r_s, risk_free=rf_bar, annualization=bpy)),
        ("sharpe_bar", "quantstats", eng["sharpe_bar"],
         _lib_value(qs.stats.sharpe, r_s - rf_bar, rf=0.0, periods=bpy)),
        ("sortino_bar", "numpy", eng["sortino_bar"], np_so),
        ("sortino_bar", "empyrical", eng["sortino_bar"],
         _lib_value(ep.sortino_ratio, r_s, required_return=rf_bar, annualization=bpy)),
        ("sortino_bar", "quantstats", eng["sortino_bar"],
         _lib_value(qs.stats.sortino, r_s - rf_bar, rf=0.0, periods=bpy)),
        ("sharpe_tv", "numpy", eng["sharpe_tv"], np_sh_tv),
        ("sharpe_tv", "empyrical", eng["sharpe_tv"],
         _lib_value(ep.sharpe_ratio, m_ret_s, risk_free=rf_month, annualization=12)),
        ("sharpe_tv", "quantstats", eng["sharpe_tv"],
         _lib_value(qs.stats.sharpe, m_ret_s - rf_month, rf=0.0, periods=12)),
        ("sortino_tv", "numpy", eng["sortino_tv"], np_so_tv),
        ("sortino_tv", "empyrical", eng["sortino_tv"],
         _lib_value(ep.sortino_ratio, m_ret_s, required_return=rf_month, annualization=12)),
        ("sortino_tv", "quantstats", eng["sortino_tv"],
         _lib_value(qs.stats.sortino, m_ret_s - rf_month, rf=0.0, periods=12)),
        ("cagr", "numpy", eng["cagr"], np_cagr),
        ("calmar", "numpy", eng["calmar"], np_calmar),
        ("recovery_factor", "numpy", eng["recovery_factor"], np_recovery),
    ]
    ctx = {"bars": n, "trades": run["total_trades"], "months": len(mr),
           "span_years": span_years, "cap": cap}
    return checks, ctx


def discover_strategies(corpus_root: Path) -> list[Path]:
    dirs = {p.parent for p in corpus_root.rglob("strategy.so")}
    dirs |= {p.parent for p in corpus_root.rglob("strategy.dylib")}
    dirs |= {p.parent for p in corpus_root.rglob("strategy.dll")}
    return sorted(dirs)


def crossvalidate_all(corpus_root: Path, ohlcv: Path) -> int:
    dirs = discover_strategies(corpus_root)
    if not dirs:
        print(f"no strategy libraries found under {corpus_root}", file=sys.stderr)
        return 2

    ran: list[str] = []
    skips: list[tuple[str, str]] = []
    mismatches: list[tuple[str, str, str, float, object]] = []
    conv_deltas: list[tuple[str, str, str, float]] = []
    nan_total = 0
    nan_by_field: dict[str, int] = {}
    # (field, source) -> (|rel|, rel, strategy)
    worst: dict[tuple[str, str], tuple[float, float, str]] = {}

    t_start = time.time()
    for d in dirs:
        name = d.relative_to(corpus_root).as_posix()
        try:
            run = run_engine(d, ohlcv, None)
        except Exception as exc:  # noqa: BLE001 — engine/config errors recorded as SKIP
            skips.append((name, f"{type(exc).__name__}: {exc}"))
            print(f"  SKIP  {name}: {exc}")
            continue
        equity = run["equity"]
        if len(equity) < 3:
            skips.append((name, f"curve too short ({len(equity)} points)"))
            print(f"  SKIP  {name}: curve too short ({len(equity)} points)")
            continue
        if not (equity > 0).all():
            skips.append((name, "non-positive equity in curve; "
                                "return-based adapters undefined"))
            print(f"  SKIP  {name}: non-positive equity in curve")
            continue

        checks, ctx = engine_convention_checks(run, declared_initial_capital(d))
        strat_worst = 0.0
        strat_nan = 0
        for field, source, ev, ov in checks:
            verdict, rel = classify(ev, ov)
            if verdict == "nan-agree":
                nan_total += 1
                nan_by_field[field] = nan_by_field.get(field, 0) + 1
                strat_nan += 1
                continue
            if verdict == "MISMATCH":
                mismatches.append((name, field, source, ev, ov))
                continue
            key = (field, source)
            if abs(rel) > worst.get(key, (-1.0,))[0]:
                worst[key] = (abs(rel), rel, name)
            strat_worst = max(strat_worst, abs(rel))
            if verdict == "CONVENTION-DELTA":
                conv_deltas.append((name, field, source, rel))
        ran.append(name)
        nan_note = f"  nan-agree={strat_nan}" if strat_nan else ""
        print(f"  ok    {name}: bars={ctx['bars']} trades={ctx['trades']} "
              f"months={ctx['months']} worst|rel|={strat_worst:.3e}{nan_note}")

    elapsed = time.time() - t_start
    print(f"\n{'=' * 78}")
    print(f"CORPUS CROSS-VALIDATION SUMMARY  ({elapsed:.1f}s)")
    print(f"{'=' * 78}")
    print(f"  strategies discovered : {len(dirs)}")
    print(f"  ran                   : {len(ran)}")
    print(f"  skipped               : {len(skips)}")
    for name, reason in skips:
        print(f"      SKIP {name}: {reason}")

    print(f"\n  per-field worst |relative delta| across the corpus "
          f"(engine-convention adapters):")
    print(f"  {'field':18s} {'source':12s} {'worst rel.delta':>16s}  strategy")
    for (field, source), (arel, rel, name) in sorted(worst.items()):
        print(f"  {field:18s} {source:12s} {rel:16.3e}  {name}")

    print(f"\n  NaN-agreement checks passed: {nan_total}"
          + (f"  ({', '.join(f'{k}={v}' for k, v in sorted(nan_by_field.items()))})"
             if nan_by_field else ""))

    if conv_deltas:
        print(f"\n  CONVENTION-DELTA rows (1e-6 < |rel| <= 1e-3) on "
              f"engine-convention adapters: {len(conv_deltas)}")
        for name, field, source, rel in conv_deltas:
            print(f"      {name}: {field}|{source} rel={rel:.3e}")

    if mismatches:
        print(f"\n  MISMATCHES ({len(mismatches)}):")
        for name, field, source, ev, ov in mismatches:
            ov_txt = repr(ov) if isinstance(ov, Exception) else f"{ov:.10g}"
            print(f"      {name}: {field}|{source} engine={ev:.10g} ref={ov_txt}")
        return 1

    print("\n  RESULT: no mismatch — every engine-convention value is "
          "reproduced by numpy/empyrical/quantstats (NaN fields agree on "
          "degeneracy).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("strategy_dir", type=Path, nargs="?", default=None,
                    help="corpus/<cat>/<name>/ containing strategy.so|.dylib "
                         "(omit with --all)")
    ap.add_argument("--all", action="store_true",
                    help="Sweep every strategy under --corpus-root instead of "
                         "a single directory (engine-convention adapters only).")
    ap.add_argument("--corpus-root", type=Path,
                    default=REPO_ROOT / "corpus" / "validation",
                    help="Root scanned by --all (default: corpus/validation)")
    ap.add_argument("--trim-end-ms", type=int, default=None,
                    help="Drop input bars with timestamp > this (Unix ms) "
                         "before the run, to test alternate spans.")
    ap.add_argument("--ohlcv", type=Path, default=rs.DEFAULT_OHLCV,
                    help="OHLCV CSV (default: the corpus default feed)")
    ap.add_argument("--initial-capital", type=float, default=None,
                    help="strategy() initial_capital (default: parsed from "
                         "generated.cpp, corpus convention 1e6; pf_report_t "
                         "does not expose it)")
    args = ap.parse_args()
    rs.ensure_derived()
    if args.all:
        return crossvalidate_all(args.corpus_root.resolve(), args.ohlcv.resolve())
    if args.strategy_dir is None:
        ap.error("strategy_dir is required unless --all is given")
    return crossvalidate(args.strategy_dir.resolve(), args.ohlcv.resolve(),
                         args.trim_end_ms, args.initial_capital)


if __name__ == "__main__":
    sys.exit(main())
