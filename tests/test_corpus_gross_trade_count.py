"""Issue #836: one unmatched trade on each side of equal-size trade lists."""

import argparse
import csv
import sys
import tempfile
from datetime import datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from verify_corpus import analyze_strategy


def write_trades(path, entries):
    with path.open("x", newline="", encoding="utf-8") as tape:
        writer = csv.writer(tape)
        writer.writerow(["Trade #", "Type", "Date and time", "Price", "Qty", "Net PnL"])
        for n, entry in enumerate(sorted(entries), 1):
            writer.writerow([n, "Entry long", entry.isoformat(sep=" ", timespec="minutes"), 100, 1, 0])
            exit_time = entry + timedelta(minutes=15)
            writer.writerow([n, "Exit long", exit_time.isoformat(sep=" ", timespec="minutes"), 110, 1, 10])


def check(strategy):
    result = analyze_strategy(strategy)
    # Require one real unmatched trade on each side and rule out a different
    # gate as the reason the probe could have been graded below Excellent.
    counts = (result.tv_gate_count, result.eng_gate_count, result.gating_matched_count)
    wanted_counts = (101, 101, 100)
    if counts != wanted_counts:
        print(f"NOT COMPARED #836: gated TV, engine, and matched counts; "
              f"expected={wanted_counts}, actual={counts}", file=sys.stderr)
        return 2
    other_gates = (result.entry_ok, result.exit_ok, result.pnl_ok,
                   result.coverage_ok, result.distinct_entry_identity_ok)
    wanted_gates = (True, True, True, True, True)
    if other_gates != wanted_gates:
        print(f"NOT COMPARED #836: entry, exit, PnL, coverage, and identity gates; "
              f"expected={wanted_gates}, actual={other_gates}", file=sys.stderr)
        return 2

    expected = (2, 2, 2 / 101, False, "strong")
    actual = (result.count_abs_delta, result.unmatched_in_window, result.count_delta,
              result.count_ok, result.tier)
    fields = "count delta, unmatched, fraction, count OK, tier"
    if actual != expected:
        print(f"FAIL #836: {fields}; expected={expected}, actual={actual}", file=sys.stderr)
        return 1
    print(f"PASS #836: {fields} = {actual}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", choices=("issue", "empty", "wrong"), default="issue")
    args = parser.parse_args()

    # The issue gives one unmatched on each side, rather than CSVs. Place
    # both between common trades and over one hour from every other entry.
    start = datetime(2025, 1, 1)
    shared = [start + timedelta(hours=12 * i) for i in range(100)]
    tv = shared + [shared[49] + timedelta(hours=4)]
    engine = shared + [shared[49] + timedelta(hours=8)]
    if args.fixture == "empty":
        tv = engine = []
    elif args.fixture == "wrong":
        engine = tv

    with tempfile.TemporaryDirectory(prefix="pineforge-issue-836-") as temp:
        strategy = Path(temp)
        (strategy / "inputs.json").write_text('{"tv_trades_csv_tz":"utc"}', encoding="utf-8")
        write_trades(strategy / "tv_trades.csv", tv)
        write_trades(strategy / "engine_trades.csv", engine)
        return check(strategy)


if __name__ == "__main__":
    sys.exit(main())
