#!/usr/bin/env python3
"""Time all available modular vectorbt strategies.

Output: JSON {strategy: {median_ms, p95_ms, n}} to stdout and/or saved to a file.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import importlib.util
from pathlib import Path
import numpy as np
import pandas as pd
import vectorbt as vbt

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH = REPO_ROOT / "benchmarks"
sys.path.insert(0, str(BENCH))
from paths import STRATEGIES, DATA  # noqa: E402

DATA_CSV = DATA / "ETHUSDT_15.csv"
DEFAULT_OUT = BENCH / "_workdir" / "vbt_speed.json"
DEFAULT_N = 20


def load_vbt_strategy(name: str) -> callable | None:
    strategy_dir = STRATEGIES / name
    file_path = strategy_dir / "strategy_vbt.py"
    if not file_path.exists():
        return None

    module_name = f"strategy_vbt_{name.replace('-', '_')}"
    try:
        spec = importlib.util.spec_from_file_location(module_name, file_path)
        if spec is None or spec.loader is None:
            return None
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        return getattr(module, "run_vbt", None)
    except Exception as e:
        print(f"Error loading {name} strategy: {e}", file=sys.stderr)
        return None


def main() -> None:
    parser = argparse.ArgumentParser(description="Time core strategies in vectorbt.")
    parser.add_argument("--n", type=int, default=DEFAULT_N,
                        help=f"Repetitions per strategy (default: {DEFAULT_N})")
    parser.add_argument("--only", default=None,
                        help="Filter: only time strategies whose name contains this string")
    parser.add_argument("--write-trades", action="store_true",
                        help="Execute each strategy and write its trade output to vectorbt_trades.csv")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help=f"Output path for JSON speed results (default: {DEFAULT_OUT})")
    args = parser.parse_args()

    if not DATA_CSV.exists():
        print(f"Error: Data file not found at {DATA_CSV}", file=sys.stderr)
        sys.exit(1)

    df = pd.read_csv(DATA_CSV)

    # Discover and load available strategies
    strategies: dict[str, callable] = {}
    for d in sorted(STRATEGIES.iterdir()):
        if not d.is_dir() or d.name.startswith("_") or d.name.startswith("."):
            continue
        if args.only and args.only not in d.name:
            continue
        run_fn = load_vbt_strategy(d.name)
        if run_fn:
            strategies[d.name] = run_fn

    if not strategies:
        print("No vectorbt strategies found or loaded.", file=sys.stderr)
        sys.exit(0)

    # Warmup Numba compiler on a tiny 100-bar slice of data for loaded strategies
    print("Warming up Numba compiler...", file=sys.stderr)
    warmup_df = df.head(100)
    for name, run_fn in strategies.items():
        try:
            _ = run_fn(warmup_df)
        except Exception as e:
            print(f"Warning: Warmup failed for {name}: {e}", file=sys.stderr)

    if args.write_trades:
        print("Writing vectorbt trades to CSV...", file=sys.stderr)
        for name, run_fn in strategies.items():
            try:
                # Run simulation with 0 fees for exact parity against TV's 0-fee trades
                portfolio = run_fn(df, fees=0.0)

                # Map indices to timestamps
                trades_df = portfolio.trades.records
                out_rows = []

                if len(trades_df) > 0:
                    for _, row in trades_df.iterrows():
                        entry_idx = int(row['entry_idx'])
                        exit_idx = int(row['exit_idx'])

                        entry_ts = df['timestamp'].iloc[entry_idx]
                        exit_ts = df['timestamp'].iloc[exit_idx]

                        entry_dt = pd.to_datetime(entry_ts, unit='ms').tz_localize('UTC')
                        exit_dt = pd.to_datetime(exit_ts, unit='ms').tz_localize('UTC')

                        entry_time_str = entry_dt.strftime('%Y-%m-%dT%H:%M:%SZ')
                        exit_time_str = exit_dt.strftime('%Y-%m-%dT%H:%M:%SZ')

                        direction = 'long' if row['direction'] == 0 else 'short'

                        out_rows.append({
                            'id': int(row['id']),
                            'direction': direction,
                            'entry_time': entry_time_str,
                            'entry_price': float(row['entry_price']),
                            'exit_time': exit_time_str,
                            'exit_price': float(row['exit_price']),
                            'pnl': float(row['pnl']),
                            'qty': float(row['size'])
                        })

                out_df = pd.DataFrame(out_rows, columns=['id', 'direction', 'entry_time', 'entry_price', 'exit_time', 'exit_price', 'pnl', 'qty'])

                strategy_dir = STRATEGIES / name
                out_path = strategy_dir / "vectorbt_trades.csv"
                out_df.to_csv(out_path, index=False)
                print(f"  {name}: Wrote {len(out_df)} trades to {out_path}", file=sys.stderr)
            except Exception as e:
                print(f"Error writing trades for {name}: {e}", file=sys.stderr)
        sys.exit(0)

    print("Running vectorbt benchmarks...", file=sys.stderr)
    vbt_results: dict[str, dict] = {}

    for name, run_fn in strategies.items():
        try:
            samples_ms = []
            for _ in range(args.n):
                t0 = time.perf_counter()
                portfolio = run_fn(df, fees=0.001)
                # Access any metric to ensure lazy calculations inside vbt portfolio are forced
                _ = portfolio.total_return()
                elapsed = (time.perf_counter() - t0) * 1000.0
                samples_ms.append(elapsed)

            arr = np.array(samples_ms)
            vbt_results[name] = {
                "median_ms": float(np.median(arr)),
                "p95_ms": float(np.percentile(arr, 95)),
                "n": args.n
            }
            print(f"  {name}: vbt_median={vbt_results[name]['median_ms']:.1f}ms", file=sys.stderr)
        except Exception as e:
            print(f"Error benchmarking {name}: {e}", file=sys.stderr)

    # Sort final output alphabetically by strategy name
    sorted_out = {k: vbt_results[k] for k in sorted(vbt_results.keys())}

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with open(args.out, "w") as f:
            json.dump(sorted_out, f, indent=2)
        print(f"Wrote speed results to {args.out}", file=sys.stderr)

    json.dump(sorted_out, sys.stdout, indent=2)
    print(file=sys.stdout)  # trailing newline


if __name__ == "__main__":
    main()
