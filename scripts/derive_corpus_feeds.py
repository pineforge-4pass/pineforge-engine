#!/usr/bin/env python3
"""Materialize the derived corpus feeds from the single committed 1m feed.

The corpus ships exactly ONE reference feed:
    corpus/data/ohlcv_ETH-USDT-USDT_1m.csv        (Git LFS)
1-minute Binance ETH-USDT-USDT perp bars, full exchange history
(2020-01-01 onward) through the end of the comparison window.

Everything else the harnesses consume is derived deterministically from
it into corpus/data/derived/ (gitignored):

    ohlcv_ETH-USDT-USDT_15m.csv         900s resample, full history —
                                        the default chart feed
    ohlcv_ETH-USDT-USDT_15m_window.csv  comparison-window slice of the
                                        above — cold-start probes and
                                        benchmark runners (bench inputs
                                        must stay historically
                                        comparable), and the harness's
                                        window-bounds fallback

Resample rule: open=first, high=max, low=min, close=last, volume=sum
(rounded to 6dp), timestamp=bucket start; a trailing partial bucket is
kept. Idempotent: files are only rewritten when missing or older than
the source feed. Import ensure_derived() or run as a script.
"""
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_1M = REPO_ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_1m.csv"
DERIVED_DIR = REPO_ROOT / "corpus" / "data" / "derived"
DERIVED_15M = DERIVED_DIR / "ohlcv_ETH-USDT-USDT_15m.csv"
DERIVED_15M_WINDOW = DERIVED_DIR / "ohlcv_ETH-USDT-USDT_15m_window.csv"

# Comparison window (epoch ms, inclusive), pinned from the historical
# window-only 15m feed the corpus used to ship: first bar 2025-04-20
# 21:00 UTC, last bar 2026-05-04 06:00 UTC bucket.
WINDOW_START_MS = 1745182800000
WINDOW_END_MS = 1777906800000

HEADER = "timestamp,open,high,low,close,volume"


def _fmt(v: float) -> str:
    """Shortest-repr float, integral values written bare (2382 not 2382.0)
    — matches the committed feed's formatting."""
    if v == int(v) and abs(v) < 1e15:
        return str(int(v))
    return repr(v)


def _stale(target: Path, source: Path) -> bool:
    return (not target.exists()
            or target.stat().st_mtime < source.stat().st_mtime)


def ensure_derived(verbose: bool = False) -> None:
    """Create/refresh the derived feeds. Cheap no-op when up to date."""
    if not SOURCE_1M.exists():
        raise FileNotFoundError(
            f"{SOURCE_1M} missing — is the corpus submodule checked out "
            "with git-lfs installed? (file should be ~176 MB, not a "
            "small LFS pointer)")
    if SOURCE_1M.stat().st_size < 1_000_000:
        raise RuntimeError(
            f"{SOURCE_1M} is suspiciously small — likely an unsmudged "
            "Git LFS pointer. Run: git lfs install && git lfs pull "
            "(inside the corpus submodule).")
    if not (_stale(DERIVED_15M, SOURCE_1M)
            or _stale(DERIVED_15M_WINDOW, SOURCE_1M)):
        return

    if verbose:
        print(f"[derive] resampling {SOURCE_1M.name} -> 15m ...")
    buckets = []  # [ts, o, h, l, c, v] per 900s bucket, in order
    cur_key = None
    with SOURCE_1M.open() as fh:
        next(fh)  # header
        for line in fh:
            ts_s, o, h, l, c, v = line.rstrip("\n").split(",")
            ts = int(ts_s)
            key = ts - ts % 900_000
            if key != cur_key:
                buckets.append([key, o, float(h), float(l), c, float(v)])
                cur_key = key
            else:
                b = buckets[-1]
                hf, lf = float(h), float(l)
                if hf > b[2]:
                    b[2] = hf
                if lf < b[3]:
                    b[3] = lf
                b[4] = c
                b[5] += float(v)

    DERIVED_DIR.mkdir(parents=True, exist_ok=True)
    full_lines = [HEADER]
    window_lines = [HEADER]
    for ts, o, h, l, c, v in buckets:
        row = f"{ts},{o},{_fmt(h)},{_fmt(l)},{c},{_fmt(round(v, 6))}"
        full_lines.append(row)
        if WINDOW_START_MS <= ts <= WINDOW_END_MS:
            window_lines.append(row)
    for path, lines in ((DERIVED_15M, full_lines),
                        (DERIVED_15M_WINDOW, window_lines)):
        tmp = path.with_suffix(".csv.new")
        tmp.write_text("\n".join(lines) + "\n")
        tmp.replace(path)
        if verbose:
            print(f"[derive] wrote {path.relative_to(REPO_ROOT)} "
                  f"({len(lines) - 1} bars)")


if __name__ == "__main__":
    ensure_derived(verbose=True)
