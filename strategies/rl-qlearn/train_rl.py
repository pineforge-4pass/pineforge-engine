#!/usr/bin/env python3
"""Train the Q-learning agent offline, then evaluate the frozen policy.

Pipeline:
  1. Load the corpus ETH-USDT-USDT 1m feed and split it chronologically
     (default 70% train / 30% test).
  2. Replay the TRAIN slice for --epochs passes. Epsilon and the learning
     rate anneal across epochs; the Q-table persists between passes via
     strategy_save_qtable / strategy_load_qtable.
  3. Evaluate the frozen policy (Greedy Mode: epsilon=0, alpha=0) on the
     train slice (in-sample) and the test slice (OUT-OF-SAMPLE), with
     commission applied. The test number is the honest profit claim.

The engine aggregates the 1m feed to 15m script bars (input_tf=1,
script_tf=15) in every pass.

Usage:
    python3 strategies/rl-qlearn/train_rl.py [--epochs 40] [--seed 7]
        [--commission 0.05] [--split 0.7] [--data path/to/1m.csv]
        [--save-qtable qtable.txt]
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SO = HERE / "strategy.so"

DATA_CANDIDATES = [
    ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_1m_warmup6m.csv",
    ROOT.parent / "pineforge-corpus" / "data" / "ohlcv_ETH-USDT-USDT_1m_warmup6m.csv",
    ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_1m.csv",
    ROOT.parent / "pineforge-corpus" / "data" / "ohlcv_ETH-USDT-USDT_1m.csv",
]

INPUT_TF = b"1"    # corpus feed resolution: 1 minute
SCRIPT_TF = b"15"  # strategy decision timeframe: 15 minutes


# ctypes mirror of <pineforge/pineforge.h> (same layout as tutorial/run.py)
class BarC(ctypes.Structure):
    _fields_ = [("open",  ctypes.c_double), ("high",   ctypes.c_double),
                ("low",   ctypes.c_double), ("close",  ctypes.c_double),
                ("volume",ctypes.c_double), ("timestamp", ctypes.c_int64)]

class TradeC(ctypes.Structure):
    _fields_ = [("entry_time",  ctypes.c_int64), ("exit_time",  ctypes.c_int64),
                ("entry_price", ctypes.c_double),("exit_price", ctypes.c_double),
                ("pnl",         ctypes.c_double),("pnl_pct",    ctypes.c_double),
                ("is_long",     ctypes.c_int),   ("max_runup",  ctypes.c_double),
                ("max_drawdown",ctypes.c_double),("qty",        ctypes.c_double)]

class _Diag(ctypes.Structure):
    _fields_ = [("sec_id", ctypes.c_int), ("feed_count", ctypes.c_int64),
                ("eval_complete_count", ctypes.c_int64),
                ("eval_partial_count",  ctypes.c_int64)]

class _Trace(ctypes.Structure):
    _fields_ = [("timestamp", ctypes.c_int64), ("bar_index", ctypes.c_int32),
                ("name_id",   ctypes.c_int32), ("value",     ctypes.c_double)]

class ReportC(ctypes.Structure):
    _fields_ = [("total_trades", ctypes.c_int),
                ("trades", ctypes.POINTER(TradeC)), ("trades_len", ctypes.c_int),
                ("net_profit", ctypes.c_double),
                ("input_bars_processed",         ctypes.c_int64),
                ("script_bars_processed",        ctypes.c_int64),
                ("security_feeds_total",         ctypes.c_int64),
                ("security_eval_complete_total", ctypes.c_int64),
                ("security_eval_partial_total",  ctypes.c_int64),
                ("magnifier_sub_bars_total",     ctypes.c_int64),
                ("magnifier_sample_ticks_total", ctypes.c_int64),
                ("input_tf_seconds",  ctypes.c_int),
                ("script_tf_seconds", ctypes.c_int),
                ("script_tf_ratio",   ctypes.c_int),
                ("needs_aggregation", ctypes.c_int),
                ("bar_magnifier_enabled", ctypes.c_int),
                ("security_diag", ctypes.POINTER(_Diag)),
                ("security_diag_len", ctypes.c_int),
                ("trace", ctypes.POINTER(_Trace)), ("trace_len", ctypes.c_int),
                ("trace_names", ctypes.POINTER(ctypes.c_char_p)),
                ("trace_names_len", ctypes.c_int)]


def load_lib() -> ctypes.CDLL:
    lib = ctypes.CDLL(str(SO))
    lib.strategy_create.argtypes  = [ctypes.c_char_p]
    lib.strategy_create.restype   = ctypes.c_void_p
    lib.run_backtest_full.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ReportC)]
    lib.strategy_free.argtypes    = [ctypes.c_void_p]
    lib.report_free.argtypes      = [ctypes.POINTER(ReportC)]
    lib.strategy_set_input.argtypes    = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.strategy_set_override.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.strategy_save_qtable.argtypes  = [ctypes.c_void_p, ctypes.c_char_p]
    lib.strategy_save_qtable.restype   = ctypes.c_int
    lib.strategy_load_qtable.argtypes  = [ctypes.c_void_p, ctypes.c_char_p]
    lib.strategy_load_qtable.restype   = ctypes.c_int
    if hasattr(lib, "strategy_get_last_error"):
        lib.strategy_get_last_error.argtypes = [ctypes.c_void_p]
        lib.strategy_get_last_error.restype  = ctypes.c_char_p
    return lib


def load_bars(path: Path):
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    n = len(rows)
    bars = (BarC * n)()
    for i, r in enumerate(rows):
        bars[i] = BarC(float(r["open"]), float(r["high"]), float(r["low"]),
                       float(r["close"]), float(r["volume"]), int(r["timestamp"]))
    return bars, n


def slice_bars(bars, start: int, end: int):
    n = end - start
    out = (BarC * n)()
    ctypes.memmove(out, ctypes.byref(bars, start * ctypes.sizeof(BarC)),
                   n * ctypes.sizeof(BarC))
    return out, n


def mirror_bars(bars, n):
    """Price-inverted copy of the feed: p -> S/p (S keeps the start level).

    Log-returns flip sign exactly, so every uptrend becomes a downtrend with
    identical magnitude. Training on alternating normal/mirrored epochs forces
    the agent to learn a direction-symmetric policy instead of memorising the
    bull regime that dominates the train window.
    """
    scale = bars[0].close * bars[0].close
    out = (BarC * n)()
    for i in range(n):
        b = bars[i]
        out[i] = BarC(scale / b.open, scale / b.low, scale / b.high,
                      scale / b.close, b.volume, b.timestamp)
    return out


def run_pass(lib, bars, n, inputs: dict, overrides: dict,
             qload: Path | None, qsave: Path | None) -> dict:
    state = lib.strategy_create(b"{}")
    for k, v in overrides.items():
        lib.strategy_set_override(state, k.encode(), str(v).encode())
    for k, v in inputs.items():
        lib.strategy_set_input(state, k.encode(), str(v).encode())
    if qload is not None:
        if lib.strategy_load_qtable(state, str(qload).encode()) != 1:
            lib.strategy_free(state)
            sys.exit(f"failed to load Q-table from {qload}")
    report = ReportC()
    lib.run_backtest_full(state, bars, n, INPUT_TF, SCRIPT_TF, 0, 4, 3,
                          ctypes.byref(report))
    if hasattr(lib, "strategy_get_last_error"):
        err_ptr = lib.strategy_get_last_error(state)
        if err_ptr and err_ptr.decode("utf-8", "replace"):
            sys.exit(f"engine error: {err_ptr.decode('utf-8', 'replace')}")
    if qsave is not None:
        if lib.strategy_save_qtable(state, str(qsave).encode()) != 1:
            sys.exit(f"failed to save Q-table to {qsave}")

    trades = [report.trades[i] for i in range(report.trades_len)]
    pnls = [t.pnl for t in trades]
    cum = peak = max_dd = 0.0
    for p in pnls:
        cum += p; peak = max(peak, cum); max_dd = min(max_dd, cum - peak)
    gross_win = sum(p for p in pnls if p > 0)
    gross_loss = -sum(p for p in pnls if p < 0)
    stats = {
        "trades": report.trades_len,
        "wins": sum(p > 0 for p in pnls),
        "losses": sum(p < 0 for p in pnls),
        "net": report.net_profit,
        "pf": (gross_win / gross_loss) if gross_loss > 0 else float("inf"),
        "max_dd": max_dd,
        "longs": sum(1 for t in trades if t.is_long),
        "script_bars": report.script_bars_processed,
    }
    lib.report_free(ctypes.byref(report))
    lib.strategy_free(state)
    return stats


def fmt_ts(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d")


def describe(label: str, s: dict, bh: float) -> None:
    win_pct = s["wins"] / s["trades"] * 100 if s["trades"] else 0.0
    print(f"  {label:<22} net {s['net']:+9.2f}  trades {s['trades']:4d} "
          f"({win_pct:4.1f}% win, {s['longs']} long)  PF {s['pf']:.3f}  "
          f"maxDD {s['max_dd']:8.2f}  [buy&hold {bh:+.2f}]")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", type=Path, default=None)
    ap.add_argument("--epochs", type=int, default=200)
    ap.add_argument("--split", type=float, default=0.7,
                    help="train fraction (chronological)")
    ap.add_argument("--seed", type=int, default=None,
                    help="single seed (overrides --seeds)")
    ap.add_argument("--seeds", type=str, default="1,3,5,7",
                    help="comma list: train one agent per seed, deploy the "
                         "best by TRAIN net (test stays untouched)")
    ap.add_argument("--commission", type=float, default=0.05,
                    help="percent commission per fill (both train + eval)")
    ap.add_argument("--gamma", type=float, default=0.998,
                    help="discount factor; needs to be near 1 so the value "
                         "of holding through a ~120-bar trend regime is "
                         "visible past the entry cost")
    ap.add_argument("--eps-hi", type=float, default=0.15)
    ap.add_argument("--eps-lo", type=float, default=0.01)
    ap.add_argument("--alpha-hi", type=float, default=0.05)
    ap.add_argument("--alpha-lo", type=float, default=0.005)
    ap.add_argument("--adapt-alpha", type=float, default=0.05,
                    help="learning rate for the adaptive (online) test pass; "
                         "0 disables it")
    ap.add_argument("--no-mirror", dest="mirror", action="store_false",
                    help="disable the price-inverted augmentation epochs "
                         "(mirror training is on by default)")
    ap.set_defaults(mirror=True)
    ap.add_argument("--save-qtable", type=Path, default=None,
                    help="keep the trained Q-table at this path")
    args = ap.parse_args()

    if not SO.exists():
        sys.exit("strategy.so missing — build it first:\n"
                 "  cmake -B build -S . -DPINEFORGE_BUILD_RL_STRATEGY=ON\n"
                 "  cmake --build build --target strategy_rl_qlearn -j")

    data = args.data or next((p for p in DATA_CANDIDATES if p.exists()), None)
    if data is None or not data.exists():
        sys.exit("1m OHLCV feed not found — init the corpus submodule or pass --data")

    lib = load_lib()
    bars, n = load_bars(data)
    k = int(n * args.split)
    train, n_train = slice_bars(bars, 0, k)
    test, n_test = slice_bars(bars, k, n)

    bh_train = train[n_train - 1].close - train[0].close
    bh_test = test[n_test - 1].close - test[0].close

    # Switch penalty == per-fill commission (charged per unit of position
    # change inside the agent) so the agent internalises costs 1:1.
    overrides = {"commission_type": "percent", "commission_value": args.commission}
    base_inputs = {"Discount Factor": args.gamma,
                   "Switch Cost Pct": args.commission}

    print(f"data:  {data.name}: {n} x 1m bars -> 15m script TF")
    print(f"train: {n_train} bars  {fmt_ts(train[0].timestamp)} -> {fmt_ts(train[n_train-1].timestamp)}"
          f"   test: {n_test} bars  {fmt_ts(test[0].timestamp)} -> {fmt_ts(test[n_test-1].timestamp)}")
    print(f"commission: {args.commission}%/fill   epochs: {args.epochs}   seed: {args.seed}")

    mirrored = mirror_bars(train, n_train) if args.mirror else None
    greedy = dict(base_inputs)
    greedy["Greedy Mode"] = "true"

    def train_one(seed: int, qfile: Path) -> dict:
        for e in range(args.epochs):
            frac = e / max(args.epochs - 1, 1)
            eps = args.eps_hi + (args.eps_lo - args.eps_hi) * frac
            alpha = args.alpha_hi + (args.alpha_lo - args.alpha_hi) * frac
            inputs = dict(base_inputs)
            inputs.update({"Epsilon Start": eps, "Epsilon Min": eps,
                           "Epsilon Decay": 1.0, "Learning Rate": alpha,
                           "Seed": seed * 1000003 + e})
            epoch_bars = mirrored if (mirrored is not None and e % 2 == 1) else train
            run_pass(lib, epoch_bars, n_train, inputs,
                     overrides, qfile if e > 0 else None, qfile)
        return run_pass(lib, train, n_train, greedy, overrides, qfile, None)

    # Model selection uses ONLY the train slice: one agent per seed, deploy
    # the best in-sample policy. The test slice is touched exactly once, by
    # the deployed policy.
    seeds = [args.seed] if args.seed is not None else \
        [int(s) for s in args.seeds.split(",")]
    t0 = time.time()
    candidates = []
    for seed in seeds:
        qfile = Path(tempfile.mkstemp(suffix=f".qtable.s{seed}.txt")[1])
        s_train = train_one(seed, qfile)
        candidates.append((s_train["net"], seed, qfile, s_train))
        describe(f"seed {seed:>3} train", s_train, bh_train)
    train_time = time.time() - t0

    best_net, best_seed, best_q, s_train = max(candidates, key=lambda c: c[0])
    print(f"\ndeploying seed {best_seed} (best train net {best_net:+.2f}); "
          f"frozen-policy test evaluation (commission {args.commission}%):")
    s_test = run_pass(lib, test, n_test, greedy, overrides, best_q, None)
    describe("test (OUT-OF-SAMPLE)", s_test, bh_test)

    if args.adapt_alpha > 0:
        # Online adaptation: greedy action selection (eps=0, deterministic)
        # but Q-updates stay on during the test pass, so the policy keeps
        # tracking the live regime. Causal — each decision uses only bars
        # already seen — so this is still an out-of-sample result.
        adapt = dict(base_inputs)
        adapt.update({"Epsilon Start": 0.0, "Epsilon Min": 0.0,
                      "Epsilon Decay": 1.0, "Learning Rate": args.adapt_alpha,
                      "Seed": best_seed})
        s_adapt = run_pass(lib, test, n_test, adapt, overrides, best_q, None)
        describe("test (OOS, adaptive)", s_adapt, bh_test)

    if args.save_qtable:
        args.save_qtable.write_text(best_q.read_text())
        print(f"deployed Q-table saved to {args.save_qtable}")
    print(f"\ntrained {len(seeds)} agents x {args.epochs} epochs in {train_time:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
