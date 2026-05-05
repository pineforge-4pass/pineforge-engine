#!/usr/bin/env python3
"""Download Binance USDT-M futures OHLCV (ETH/USDT:USDT 15m) covering the
full TV-export time range, write to `_workdir/data/ETHUSDT_15.csv`, and
convert to PyneCore's `.ohlcv` format.

Mirrors the parent project's `scripts/fetch_data.py` (same exchange,
same ccxt provider, same 15m timeframe) so the fetched feed is
byte-identical to the corpus reference where the time ranges overlap.

The default `--since` is **2025-03-01**, ~30 days before TV's earliest
trade (2025-03-31), giving every benchmark strategy ample warmup
before its first compared entry. The default `--to` is "now" which
extends past TV's last export (2026-04-05).

Usage:
    python benchmarks/runners/fetch_extended_ohlcv.py
    python benchmarks/runners/fetch_extended_ohlcv.py --since 2025-02-15
    python benchmarks/runners/fetch_extended_ohlcv.py --no-convert  # skip pyne convert
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import ccxt
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH_DIR = REPO_ROOT / "benchmarks"
WORKDIR = BENCH_DIR / "_workdir"
OUT_CSV = WORKDIR / "data" / "ETHUSDT_15.csv"


def fetch(symbol: str, timeframe: str, since_iso: str) -> pd.DataFrame:
    """Pull all bars from `since_iso` (ISO-8601 UTC) through now."""
    exchange = ccxt.binanceusdm({"options": {"defaultType": "swap"}})
    bar_ms = int(exchange.parse_timeframe(timeframe) * 1000)
    now_ms = exchange.milliseconds()
    since_ms = exchange.parse8601(f"{since_iso}T00:00:00Z")

    rows: list[list] = []
    cursor = since_ms
    while cursor < now_ms:
        try:
            chunk = exchange.fetch_ohlcv(symbol, timeframe, since=cursor, limit=1500)
        except ccxt.base.errors.BadRequest:
            cursor += 1500 * bar_ms
            continue
        if not chunk:
            break
        rows.extend(chunk)
        last = chunk[-1][0]
        if last <= cursor:
            break
        cursor = last + 1
        print(f"  fetched {len(rows)} bars (last: {exchange.iso8601(last)})")
        time.sleep(0.2)

    df = pd.DataFrame(rows, columns=["timestamp", "open", "high", "low", "close", "volume"])
    df = df.drop_duplicates(subset=["timestamp"]).sort_values("timestamp").reset_index(drop=True)
    return df


def convert_to_pyne(csv_path: Path) -> None:
    """Invoke `pyne data convert-from` to make the .ohlcv companion."""
    cmd = [
        "pyne", "-w", str(WORKDIR.resolve()),
        "data", "convert-from",
        "--provider", "pineforge",
        "--symbol", "ETHUSDT",
        "--timezone", "UTC",
        str(csv_path.resolve()),
    ]
    subprocess.run(cmd, check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbol", default="ETH/USDT:USDT",
                    help="ccxt symbol (default ETH perpetual)")
    ap.add_argument("--timeframe", default="15m")
    ap.add_argument("--since", default="2025-03-01",
                    help="UTC start date YYYY-MM-DD")
    ap.add_argument("--no-convert", action="store_true",
                    help="Skip the .ohlcv conversion step")
    args = ap.parse_args()

    OUT_CSV.parent.mkdir(parents=True, exist_ok=True)

    print(f"fetching {args.symbol} {args.timeframe} from {args.since} via Binance USDT-M")
    df = fetch(args.symbol, args.timeframe, args.since)
    df.to_csv(OUT_CSV, index=False)
    first = pd.to_datetime(int(df["timestamp"].iloc[0]), unit="ms", utc=True)
    last = pd.to_datetime(int(df["timestamp"].iloc[-1]), unit="ms", utc=True)
    print(f"wrote {len(df)} bars to {OUT_CSV}")
    print(f"  range: {first} → {last}")

    if not args.no_convert:
        # Replace the existing .ohlcv (if any) so pyne run uses fresh bars.
        ohlcv_path = OUT_CSV.with_suffix(".ohlcv")
        if ohlcv_path.exists():
            ohlcv_path.unlink()
        toml_path = OUT_CSV.with_suffix(".toml")
        if toml_path.exists():
            toml_path.unlink()
        convert_to_pyne(OUT_CSV)
    return 0


if __name__ == "__main__":
    sys.exit(main())
