#!/usr/bin/env python3
"""Refresh tutorial/data/btcusdt_15m_7d.csv with the latest 7 days of
Binance BTCUSDT 15m klines (672 bars, no auth required).

Usage:
    python3 tutorial/data/fetch_btcusdt.py [--symbol BTCUSDT] [--limit 672]
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

BINANCE_URL = "https://api.binance.com/api/v3/klines"
DEFAULT_OUT = Path(__file__).resolve().parent / "btcusdt_15m_7d.csv"


def fetch(symbol: str, interval: str, limit: int) -> list[list]:
    url = f"{BINANCE_URL}?symbol={symbol}&interval={interval}&limit={limit}"
    with urllib.request.urlopen(url, timeout=30) as resp:
        return json.loads(resp.read())


def write_csv(rows: list[list], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["timestamp", "open", "high", "low", "close", "volume"])
        for k in rows:
            w.writerow([int(k[0]), k[1], k[2], k[3], k[4], k[5]])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbol",   default="BTCUSDT")
    ap.add_argument("--interval", default="15m")
    ap.add_argument("--limit",    type=int, default=672,
                    help="Number of bars (672 = 7 days at 15m).")
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    rows = fetch(args.symbol, args.interval, args.limit)
    write_csv(rows, args.output)

    first_ts = int(rows[0][0]) // 1000
    last_ts = int(rows[-1][0]) // 1000
    fmt = lambda ts: datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    print(f"wrote {len(rows)} bars  {fmt(first_ts)} -> {fmt(last_ts)}")
    print(f"  {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
