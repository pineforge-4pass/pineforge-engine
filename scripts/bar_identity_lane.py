#!/usr/bin/env python3
"""L0 bar-identity lane, row 1: engine aggregate(1m) vs the graded derived 15m
chart feed (spec §10.1). Fields compared as parsed doubles; volume within 1e-6.

Expected divergence on ETH.P: the derive rule (scripts/derive_corpus_feeds.py)
opens a bucket on its first POSITIVE-volume row (or the first row, if the
whole bucket is zero-volume), while the engine's TimeframeAggregator opens a
bucket on its true first row unconditionally -- a leading run of zero-volume
minutes is the one documented case where the two disagree on `open`. `high`/
`low`/`close` are expected identical: both aggregations fold every row's
high/low/close (max/min/last) regardless of volume.

This script doesn't just count divergences -- every one is checked against
the bucket's own 1m source rows and classified as *_explained_* (matches the
documented rule above) or *_unexplained (anything else). A non-empty
*_unexplained bucket is the lane's actual signal: it means the two
aggregations disagree for a reason the derive rule does not predict, and the
script reports the rows and exits 1 rather than being tweaked to pass.
`volume` divergences and `missing_in_aggregate`/`missing_in_derived` buckets
are unexplained by construction (no rule predicts either) and also count
toward that exit-1 signal.
"""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ONE_M = ROOT / "corpus/data/ohlcv_ETH-USDT-USDT_1m.csv"
DERIVED = ROOT / "corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv"
AGG = ROOT / "build/bin/aggregate_feed"
OUT = ROOT / "build/bar_identity_row1.json"

TF = "15"

MIN_BARS_COMPARED = 100_000
MAX_EXAMPLES = 20

FIELDS = ("open", "high", "low", "close")


def build_arg_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--one-m", type=Path, default=ONE_M,
                     help="1m OHLCV source CSV (default: %(default)s)")
    ap.add_argument("--derived", type=Path, default=DERIVED,
                     help="derived feed CSV to compare the aggregate against (default: %(default)s)")
    ap.add_argument("--tf", default=TF,
                     help="target timeframe, in integer minutes, passed straight to "
                          "aggregate_feed (default: %(default)s)")
    ap.add_argument("--out", type=Path, default=OUT,
                     help="output JSON path (default: %(default)s)")
    ap.add_argument("--out-csv", type=Path, default=None,
                     help="aggregate_feed's intermediate CSV output path "
                          "(default: build/aggregate_<tf>m.csv, derived from --tf so "
                          "concurrent --tf runs don't clobber each other's scratch file)")
    ap.add_argument("--aggregate-bin", type=Path, default=AGG,
                     help="path to the built aggregate_feed binary (default: %(default)s)")
    return ap


def _parse_kv_line(line: str) -> dict[str, str]:
    """Split a `key=value key2=value2 ...` stderr line (an optional leading
    `label:` token is ignored -- it has no `=`) into a dict."""
    return dict(tok.split("=", 1) for tok in line.split() if "=" in tok)


REQUIRED_COLUMNS = ("timestamp", "open", "high", "low", "close", "volume")


def _require_header(p: Path, reader: csv.DictReader) -> None:
    """N3: both CSV loaders share one header contract with the tool side
    (aggregate_feed now requires the same six columns, finding 1/N1) -- a
    header-less or malformed feed dies with a clear message here instead of
    a raw `KeyError: 'timestamp'` traceback (task-12-rereview finding 3)."""
    fieldnames = set(reader.fieldnames or ())
    missing = [c for c in REQUIRED_COLUMNS if c not in fieldnames]
    if missing:
        sys.exit(f"error: {p} has no {','.join(REQUIRED_COLUMNS)} header "
                  f"(missing: {','.join(missing)})")


def load(p: Path) -> dict[int, dict]:
    with p.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        _require_header(p, reader)
        return {
            int(r["timestamp"]): {k: float(r[k]) for k in ("open", "high", "low", "close", "volume")}
            for r in reader
        }


def load_1m_row_count_and_divergent_buckets(
    p: Path, divergent_ts: set[int], bucket_ms: int
) -> tuple[int, dict[int, list[dict]]]:
    """One pass over the (3.3M-row) 1m CSV: count every row (for the
    non-vacuity check, and to cross-check the tool's own rows_parsed receipt)
    and, in the same pass, collect the source rows for only the (typically
    small) set of already-known divergent bucket starts -- classification
    never re-scans the file per divergence."""
    n = 0
    buckets: dict[int, list[dict]] = {ts: [] for ts in divergent_ts}
    with p.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        _require_header(p, reader)
        for r in reader:
            n += 1
            if not divergent_ts:
                continue
            row_ts = int(r["timestamp"])
            bucket_start = row_ts - (row_ts % bucket_ms)
            bucket = buckets.get(bucket_start)
            if bucket is not None:
                bucket.append({
                    "timestamp": row_ts,
                    "open": float(r["open"]), "high": float(r["high"]),
                    "low": float(r["low"]), "close": float(r["close"]),
                    "volume": float(r["volume"]),
                })
    return n, buckets


def classify_open(bucket_rows: list[dict], agg_open: float, derived_open: float) -> str:
    """derive rule (scripts/derive_corpus_feeds.py _resample_15m): open = the
    bucket's first POSITIVE-volume row's open (or the first row's, if the
    whole bucket is zero-volume). Engine (feed_reset_current /
    feed_merge_into_current, src/timeframe.cpp:1026,1065-1074): open = the
    bucket's true first row's open, always. A divergence is explained only
    when the bucket opens with a run of zero-volume rows followed by a
    positive-volume one, and the two aggregations' opens match that rule
    exactly."""
    if not bucket_rows:
        return "open_unexplained"
    first = bucket_rows[0]
    first_positive = next((r for r in bucket_rows if r["volume"] > 0), None)
    if (first["volume"] == 0.0 and first_positive is not None
            and agg_open == first["open"] and derived_open == first_positive["open"]):
        return "open_explained_leading_zero_volume"
    return "open_unexplained"


def classify_hl_close(field: str, bucket_rows: list[dict], agg_val: float, derived_val: float) -> str:
    """high/low/close are expected identical between the two aggregations
    (both fold every row regardless of volume: high=max, low=min,
    close=last). close never depends on volume at all, so a close
    divergence has no explained bucket.

    As strict as classify_open: a high/low divergence is explained only when
    ALL THREE hold -- the aggregate's value equals the extreme over every
    row in the bucket, the derived value equals the extreme over only the
    positive-volume rows, and the two extremes actually differ. Under the
    derive rule this script currently knows about (scripts/derive_corpus_feeds.py
    :127-131 folds high/low over every row regardless of volume, same as the
    engine's feed_merge_into_current, src/timeframe.cpp:1079-1086), those two
    extremes can never differ -- so this bucket is unreachable today. That is
    the honest state, not a bug: it stays unreachable until the derive rule
    itself changes to something volume-sensitive."""
    if field in ("high", "low") and bucket_rows:
        reducer = max if field == "high" else min
        extreme_all = reducer(r[field] for r in bucket_rows)
        positive_rows = [r for r in bucket_rows if r["volume"] > 0]
        if positive_rows:
            extreme_positive = reducer(r[field] for r in positive_rows)
            if (agg_val == extreme_all and derived_val == extreme_positive
                    and extreme_all != extreme_positive):
                return "hl_explained_zero_volume_row"
    return f"{field}_unexplained"


def main() -> int:
    args = build_arg_parser().parse_args()

    if not args.aggregate_bin.exists():
        sys.exit(f"error: {args.aggregate_bin} not built -- "
                  f"run: cmake --build build -j8 --target aggregate_feed")

    try:
        bucket_ms = int(args.tf) * 60 * 1000
    except ValueError:
        sys.exit(f"error: --tf {args.tf!r} must be an integer number of minutes")

    # N4: per-tf default so a --tf 60 run doesn't write into a file named
    # 15m, and two scratch runs at different --tf don't clobber each other's
    # intermediate (task-12-rereview finding 4).
    out_csv = args.out_csv if args.out_csv is not None else ROOT / f"build/aggregate_{args.tf}m.csv"

    proc = subprocess.run(
        [str(args.aggregate_bin), str(args.one_m), args.tf, str(out_csv)],
        capture_output=True, text=True,
    )
    trailing_partial = 0
    rows_parsed: int | None = None
    rows_skipped: int | None = None
    first_skipped_line: int | None = None
    aggregator_info: dict[str, str] = {}
    for line in proc.stderr.splitlines():
        if line.startswith("rows_parsed="):
            kv = _parse_kv_line(line)
            rows_parsed = int(kv.get("rows_parsed", "0"))
            rows_skipped = int(kv.get("rows_skipped", "0"))
            trailing_partial = int(kv.get("trailing_partial", "0"))
            if "first_skipped_line" in kv:
                first_skipped_line = int(kv["first_skipped_line"])
        elif line.startswith("aggregator:"):
            aggregator_info = _parse_kv_line(line)
    if proc.returncode != 0:
        # N5: relay the tool's first_skipped_line (1-based) receipt, when it
        # reported one, so a caller doesn't have to re-scan the source file
        # to find the one bad row (task-12-rereview finding 5).
        line_note = f" (first_skipped_line={first_skipped_line})" if first_skipped_line else ""
        sys.exit(f"error: {args.aggregate_bin} exited {proc.returncode}{line_note}\n"
                  f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")

    # N1: the tool's aggregator provenance line is a hard requirement, not a
    # soft fallback -- an absent (or malformed) line means a stale/pre-
    # provenance binary and this lane must not silently assert the
    # aggregator config from an unrelated constant (task-12-rereview new
    # finding 1). rows_parsed is checked the same way below (pre-existing).
    _missing_agg_keys = [k for k in ("tf", "input_tf", "tz", "session") if k not in aggregator_info]
    if _missing_agg_keys:
        sys.exit(f"error: {args.aggregate_bin} did not report aggregator provenance on stderr "
                  f"(missing: {','.join(_missing_agg_keys)})")

    a, d = load(out_csv), load(args.derived)

    bars_compared = len(set(a) & set(d))
    agg_row_count = len(a)
    derived_row_count = len(d)

    counts = {"missing_in_aggregate": 0, "missing_in_derived": 0,
              "open": 0, "high": 0, "low": 0, "close": 0, "volume": 0}
    examples: list[dict] = []
    all_ts = sorted(set(a) | set(d))

    # First pass (over the small ~222k-row aggregate/derived dicts only):
    # find which bucket timestamps have any OHLC divergence, so the one
    # expensive pass over the 3.3M-row 1m file (below) only has to collect
    # rows for those buckets, not re-scan the file per divergence. Also
    # collects an example dump for every missing bucket -- expected zero,
    # not a 1%-of-rows tolerance (finding 4).
    divergent_ts: set[int] = set()
    missing_examples: list[dict] = []
    for ts in all_ts:
        if ts not in a:
            counts["missing_in_aggregate"] += 1
            if len(missing_examples) < MAX_EXAMPLES:
                missing_examples.append({"ts": ts, "missing_from": "aggregate", "derived": d[ts]})
            continue
        if ts not in d:
            counts["missing_in_derived"] += 1
            if len(missing_examples) < MAX_EXAMPLES:
                missing_examples.append({"ts": ts, "missing_from": "derived", "aggregate": a[ts]})
            continue
        av, dv = a[ts], d[ts]
        if any(av[k] != dv[k] for k in FIELDS):
            divergent_ts.add(ts)

    one_m_rows, bucket_rows_by_ts = load_1m_row_count_and_divergent_buckets(args.one_m, divergent_ts, bucket_ms)

    # Row receipt cross-check (finding 1): the tool's own parsed-row count
    # must match the lane's independent count of the same file, or a silent
    # drop on either side would go unnoticed.
    if rows_parsed is None:
        sys.exit(f"error: {args.aggregate_bin} did not report rows_parsed on stderr")
    if rows_parsed != one_m_rows:
        sys.exit(f"error: {args.aggregate_bin} reported rows_parsed={rows_parsed} but the lane "
                  f"independently counted {one_m_rows} data rows in {args.one_m} "
                  f"(tool rows_skipped={rows_skipped})")

    # Non-vacuity (brief step 3 + controller ruling 6): this lane exists to
    # publish counts over the whole ETH.P corpus, not a slice of it.
    if bars_compared <= MIN_BARS_COMPARED:
        sys.exit(f"error: only {bars_compared} bars compared (need > {MIN_BARS_COMPARED}); "
                  f"1m rows={one_m_rows}, aggregate rows={agg_row_count}, derived rows={derived_row_count}")

    classification = {
        "open_explained_leading_zero_volume": 0, "open_unexplained": 0,
        "hl_explained_zero_volume_row": 0,
        "high_unexplained": 0, "low_unexplained": 0, "close_unexplained": 0,
        "volume_unexplained": 0, "missing_unexplained": 0,
    }
    unexplained_examples: dict[str, list[dict]] = {
        "open_unexplained": [], "high_unexplained": [], "low_unexplained": [], "close_unexplained": [],
        "volume_unexplained": [], "missing_unexplained": [],
    }
    classification["missing_unexplained"] = counts["missing_in_aggregate"] + counts["missing_in_derived"]
    unexplained_examples["missing_unexplained"] = missing_examples

    for ts in sorted(divergent_ts):
        av, dv = a[ts], d[ts]
        bucket_rows = bucket_rows_by_ts.get(ts, [])
        for k in FIELDS:
            if av[k] == dv[k]:
                continue
            counts[k] += 1
            if len(examples) < MAX_EXAMPLES:
                examples.append({"ts": ts, "field": k, "aggregate": av[k], "derived": dv[k]})
            if k == "open":
                cls = classify_open(bucket_rows, av["open"], dv["open"])
            else:
                cls = classify_hl_close(k, bucket_rows, av[k], dv[k])
            classification[cls] = classification.get(cls, 0) + 1
            if cls.endswith("_unexplained") and len(unexplained_examples[cls]) < MAX_EXAMPLES:
                unexplained_examples[cls].append({
                    "ts": ts, "aggregate": av[k], "derived": dv[k], "rows": bucket_rows,
                })

    # volume divergences are unexplained by construction -- no rule predicts
    # one -- so they count toward the exit-1 signal too (finding 4).
    for ts in all_ts:
        if ts not in a or ts not in d:
            continue
        av, dv = a[ts], d[ts]
        if abs(av["volume"] - dv["volume"]) > 1e-6:
            counts["volume"] += 1
            if len(examples) < MAX_EXAMPLES:
                examples.append({"ts": ts, "field": "volume", "aggregate": av["volume"], "derived": dv["volume"]})
            classification["volume_unexplained"] += 1
            if len(unexplained_examples["volume_unexplained"]) < MAX_EXAMPLES:
                unexplained_examples["volume_unexplained"].append(
                    {"ts": ts, "aggregate": av["volume"], "derived": dv["volume"]})

    # Provenance (finding 5, tightened by N1): relay exactly what the tool
    # reported constructing the aggregator with -- no constant fallback; the
    # hard-fail above already guarantees these four keys are present.
    aggregator = {k: aggregator_info[k] for k in ("tf", "input_tf", "tz", "session")}

    result = {
        "bars_compared": bars_compared,
        "one_m_rows": one_m_rows,
        "aggregate_rows": agg_row_count,
        "derived_rows": derived_row_count,
        "rows_parsed": rows_parsed,
        "rows_skipped": rows_skipped,
        "first_skipped_line": first_skipped_line,
        "trailing_partial": trailing_partial,
        "aggregator": aggregator,
        "divergence": counts,
        "classification": classification,
        "examples": examples,
        "unexplained_examples": unexplained_examples,
    }
    args.out.write_text(json.dumps(result, indent=2))

    print(json.dumps({"bars_compared": bars_compared, **counts, "classification": classification,
                       "trailing_partial": trailing_partial}))

    unexplained_total = sum(v for k, v in classification.items() if k.endswith("_unexplained"))
    if unexplained_total:
        print(f"bar_identity_lane: {unexplained_total} unexplained divergence(s) -- "
              f"see {args.out} unexplained_examples", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
