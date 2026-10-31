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

Resample rule: open=first positive-volume row (or first row when the
bucket is entirely zero-volume), high=max, low=min, close=last,
volume=sum (rounded to 6dp), timestamp=bucket start; a trailing partial
bucket is kept. Idempotent: files are only rewritten when missing,
older than the source feed, or stamped with an older derivation rule.
Import ensure_derived() or run as a script.
"""
import math
import os
import tempfile
from pathlib import Path

from parity_paths import resolve_paths


_DEFAULT_PATHS = resolve_paths()
REPO_ROOT = _DEFAULT_PATHS.repo_root
SOURCE_1M = (_DEFAULT_PATHS.corpus_root / "data"
             / "ohlcv_ETH-USDT-USDT_1m.csv")
DERIVED_DIR = _DEFAULT_PATHS.derived_workspace
DERIVED_15M = DERIVED_DIR / "ohlcv_ETH-USDT-USDT_15m.csv"
DERIVED_15M_WINDOW = DERIVED_DIR / "ohlcv_ETH-USDT-USDT_15m_window.csv"
DERIVATION_STAMP = DERIVED_DIR / ".derive_corpus_feeds.version"

# Bump whenever the materialization semantics change. This invalidates
# already-generated feeds even when the committed 1m source is unchanged.
DERIVATION_CACHE_VERSION = "v2:first-positive-volume-open"

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


def _derived_cache_is_fresh(source: Path, targets, stamp: Path) -> bool:
    """Return whether outputs match both the source and derivation rule."""
    if any(_stale(target, source) for target in targets):
        return False
    try:
        return stamp.read_text().strip() == DERIVATION_CACHE_VERSION
    except OSError:
        return False


def _parse_ohlcv_row(line: str, row_number: int):
    fields = line.rstrip("\r\n").split(",")
    if len(fields) != 6:
        raise ValueError(
            f"row {row_number}: expected 6 comma-separated fields, "
            f"got {len(fields)}")

    ts_s, o, h, l, c, v = fields
    try:
        ts = int(ts_s)
    except ValueError as exc:
        raise ValueError(
            f"row {row_number}: invalid timestamp {ts_s!r}") from exc

    values = {}
    for name, text in (("open", o), ("high", h), ("low", l),
                       ("close", c), ("volume", v)):
        try:
            value = float(text)
        except ValueError as exc:
            raise ValueError(
                f"row {row_number}: invalid {name} {text!r}") from exc
        if not math.isfinite(value):
            raise ValueError(
                f"row {row_number}: {name} must be finite, got {text!r}")
        values[name] = value

    if values["volume"] < 0.0:
        raise ValueError(
            f"row {row_number}: volume must be non-negative, got {v!r}")
    return (ts, o, values["high"], values["low"], c,
            values["volume"])


def _resample_15m(lines):
    """Aggregate header-free 1m CSV rows into 15m buckets."""
    buckets = []  # [ts, o, h, l, c, v] per 900s bucket, in order
    cur_key = None
    has_positive_volume = False
    for row_number, line in enumerate(lines, start=2):
        ts, o, hf, lf, c, vf = _parse_ohlcv_row(line, row_number)
        key = ts - ts % 900_000
        if key != cur_key:
            buckets.append([key, o, hf, lf, c, vf])
            cur_key = key
            has_positive_volume = vf > 0.0
        else:
            b = buckets[-1]
            if not has_positive_volume and vf > 0.0:
                b[1] = o
                has_positive_volume = True
            if hf > b[2]:
                b[2] = hf
            if lf < b[3]:
                b[3] = lf
            b[4] = c
            b[5] += vf
    return buckets


def _format_bucket(bucket) -> str:
    ts, o, h, l, c, v = bucket
    return f"{ts},{o},{_fmt(h)},{_fmt(l)},{c},{_fmt(round(v, 6))}"


def _write_atomic(path: Path, text: str) -> None:
    # Every checkout rebuilds once when DERIVATION_CACHE_VERSION changes.  Use
    # a private same-directory temporary so two independently launched
    # validators cannot race over one fixed ``.new`` path.  Same-directory
    # replacement preserves the atomic rename guarantee.
    fd, tmp_name = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".new",
        text=True,
    )
    tmp = Path(tmp_name)
    try:
        with os.fdopen(fd, "w") as fh:
            fh.write(text)
        os.chmod(tmp, path.stat().st_mode & 0o777 if path.exists() else 0o644)
        tmp.replace(path)
    finally:
        try:
            tmp.unlink()
        except FileNotFoundError:
            pass


def ensure_derived(verbose: bool = False, *, corpus_root=None,
                   derived_workspace=None) -> None:
    """Create/refresh the derived feeds. Cheap no-op when up to date."""
    if corpus_root is None and derived_workspace is None:
        paths = _DEFAULT_PATHS
        source_1m = SOURCE_1M
        derived_dir = DERIVED_DIR
        derived_15m = DERIVED_15M
        derived_15m_window = DERIVED_15M_WINDOW
        derivation_stamp = DERIVATION_STAMP
    else:
        paths = resolve_paths(corpus_root=corpus_root,
                              derived_workspace=derived_workspace)
        source_1m = (paths.corpus_root / "data"
                     / "ohlcv_ETH-USDT-USDT_1m.csv")
        derived_dir = paths.derived_workspace
        derived_15m = derived_dir / "ohlcv_ETH-USDT-USDT_15m.csv"
        derived_15m_window = (
            derived_dir / "ohlcv_ETH-USDT-USDT_15m_window.csv")
        derivation_stamp = derived_dir / ".derive_corpus_feeds.version"

    if not source_1m.exists():
        raise FileNotFoundError(
            f"{source_1m} missing — is the corpus submodule checked out "
            "with git-lfs installed? (file should be ~176 MB, not a "
            "small LFS pointer)")
    if source_1m.stat().st_size < 1_000_000:
        raise RuntimeError(
            f"{source_1m} is suspiciously small — likely an unsmudged "
            "Git LFS pointer. Run: git lfs install && git lfs pull "
            "(inside the corpus submodule).")
    outputs = (derived_15m, derived_15m_window)
    if _derived_cache_is_fresh(source_1m, outputs, derivation_stamp):
        return

    if verbose:
        print(f"[derive] resampling {source_1m.name} -> 15m ...")
    with source_1m.open() as fh:
        next(fh)  # header
        buckets = _resample_15m(fh)

    derived_dir.mkdir(parents=True, exist_ok=True)
    full_lines = [HEADER]
    window_lines = [HEADER]
    for bucket in buckets:
        row = _format_bucket(bucket)
        full_lines.append(row)
        ts = bucket[0]
        if WINDOW_START_MS <= ts <= WINDOW_END_MS:
            window_lines.append(row)
    for path, lines in ((derived_15m, full_lines),
                        (derived_15m_window, window_lines)):
        _write_atomic(path, "\n".join(lines) + "\n")
        if verbose:
            try:
                display_path = path.relative_to(paths.repo_root)
            except ValueError:
                display_path = path
            print(f"[derive] wrote {display_path} "
                  f"({len(lines) - 1} bars)")
    # Publish the cache version only after both feed replacements succeed.
    _write_atomic(derivation_stamp, DERIVATION_CACHE_VERSION + "\n")


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus-root", type=Path, default=None,
                    help="Mounted corpus root (default: checkout corpus/).")
    ap.add_argument("--derived-workspace", type=Path, default=None,
                    help="Writable derived-feed workspace.")
    ap.add_argument("--harness-dir", type=Path, default=None,
                    help="Installed harness directory (accepted for bundle-wide CLI parity).")
    args = ap.parse_args()
    ensure_derived(verbose=True, corpus_root=args.corpus_root,
                   derived_workspace=args.derived_workspace)
