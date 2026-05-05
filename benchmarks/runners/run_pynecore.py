#!/usr/bin/env python3
"""Drive PyneCore on a benchmark strategy and emit `pynecore_trades.csv`
in the same TV-format schema PineForge uses.

Usage:
    python benchmarks/runners/run_pynecore.py benchmarks/strategies/01-sma-cross

The strategy folder must contain:
    strategy_pyne.py   — hand-ported PyneCore script
    tv_trades.csv      — ground truth (already there for column-detect)

Output:
    {strategy_dir}/pynecore_trades.csv  — TV-schema trade list
    {strategy_dir}/pynecore_stats.csv   — strategy stats (verbatim from pyne)

The CLI invokes the locally-installed `pyne run` against the corpus
OHLCV (`corpus/data/ohlcv_ETH-USDT-USDT_15m.csv`, pre-converted to
PyneCore's `.ohlcv` format under `benchmarks/_workdir/data/`). It then
re-emits the resulting trade list in PineForge's TV-mirror schema —
same column names, same exit-then-entry row order, same reverse-
chronological trade numbering — so the comparator can diff the three
engines line-for-line.
"""
from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH_DIR = REPO_ROOT / "benchmarks"
WORKDIR = BENCH_DIR / "_workdir"
OHLCV_PYNE = WORKDIR / "data" / "ETHUSDT_15.ohlcv"


def parse_iso(s: str) -> int:
    """Convert ISO-8601 string to unix-ms int."""
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    if "+" not in s and s.count("-") >= 2:
        s = s + "+00:00"
    dt = datetime.fromisoformat(s)
    return int(dt.timestamp() * 1000)


def fmt_utc(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")


def normalize_pyne_trades(pyne_csv: Path, out_csv: Path) -> int:
    """Re-emit PyneCore's trade list in the TV-mirror schema PineForge uses.

    PyneCore CSV columns:
        Trade #, Bar Index, Type, Signal, Date/Time, Price USDT, Contracts,
        Profit USDT, Profit %, Cumulative profit USDT, Cumulative profit %,
        Run-up USDT, Run-up %, Drawdown USDT, Drawdown %

    PineForge target schema:
        Trade #, Type, Date and time, Price, Qty, Net PnL, Net PnL %,
        MFE, MAE, Cumulative PnL
    """
    by_num: dict[int, dict[str, dict]] = {}
    with pyne_csv.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            n = int(row["Trade #"])
            slot = by_num.setdefault(n, {})
            kind = row["Type"]
            entry = {
                "type": kind,
                "time_ms": parse_iso(row["Date/Time"]),
                "price": float(row["Price USDT"]),
                "qty": float(row["Contracts"]),
                "pnl": float(row["Profit USDT"]),
                "pnl_pct": float(row["Profit %"]),
                "mfe": float(row.get("Run-up USDT") or 0),
                "mae": float(row.get("Drawdown USDT") or 0),
                "cum": float(row.get("Cumulative profit USDT") or 0),
            }
            slot["entry" if kind.startswith("Entry") else "exit"] = entry

    with out_csv.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "Trade #", "Type", "Date and time", "Price", "Qty",
            "Net PnL", "Net PnL %", "MFE", "MAE", "Cumulative PnL",
        ])
        for n in sorted(by_num.keys(), reverse=True):
            slot = by_num[n]
            if "entry" not in slot or "exit" not in slot:
                continue
            entry = slot["entry"]
            exit_ = slot["exit"]
            for side in (exit_, entry):
                w.writerow([
                    n, side["type"],
                    fmt_utc(side["time_ms"]),
                    f"{side['price']:.6f}",
                    f"{side['qty']:g}",
                    f"{side['pnl']:.6f}",
                    f"{side['pnl_pct']:.4f}",
                    f"{side['mfe']:.6f}",
                    f"{side['mae']:.6f}",
                    f"{side['cum']:.6f}",
                ])
    return len(by_num)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("strategy_dir", type=Path)
    ap.add_argument("--ohlcv", type=Path, default=OHLCV_PYNE,
                    help=f"PyneCore OHLCV file (default: {OHLCV_PYNE})")
    ap.add_argument("--workdir", type=Path, default=WORKDIR)
    ap.add_argument("--script", default="strategy_pyne.py",
                    help="Filename of the @pyne script inside strategy_dir")
    args = ap.parse_args()

    strat_dir = args.strategy_dir.resolve()
    script = strat_dir / args.script
    if not script.exists():
        print(f"ERROR: script not found: {script}", file=sys.stderr)
        return 1
    if not args.ohlcv.exists():
        print(f"ERROR: OHLCV file not found: {args.ohlcv}", file=sys.stderr)
        return 1

    raw_trades = strat_dir / "_pynecore_raw_trades.csv"
    raw_stats = strat_dir / "pynecore_stats.csv"

    cmd = [
        "pyne", "-w", str(args.workdir.resolve()),
        "run", str(script), str(args.ohlcv.resolve()),
        "--trade", str(raw_trades.resolve()),
        "--strat", str(raw_stats.resolve()),
    ]
    print(f"running: {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"pyne failed (rc={res.returncode}):\n{res.stderr}", file=sys.stderr)
        return res.returncode

    if not raw_trades.exists():
        print(f"ERROR: pyne did not produce a trade CSV at {raw_trades}", file=sys.stderr)
        return 1

    out_trades = strat_dir / "pynecore_trades.csv"
    n = normalize_pyne_trades(raw_trades, out_trades)
    print(f"{strat_dir.relative_to(REPO_ROOT)}: {n} trades -> pynecore_trades.csv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
