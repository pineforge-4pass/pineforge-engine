"""Run the wrapped public codec against strict-reader fragment expectations."""
from __future__ import annotations

import argparse
import hashlib
from itertools import zip_longest
from pathlib import Path
import subprocess
import tempfile

import native_sink_review_support as review


MODES = ("partial", "exact_equity", "exact_trades", "source_refusal",
         "unsupported", "deferred_result")
WINDOW = 65536
E_ROWS = 8000
T_ROWS = 1600


def values(tag, n, *, exact=False):
    numbers = (-0.0, 5e-324, 1e-05, 1e+16, 10001.25)
    price = 1.0 if exact else numbers[(n - 1) % len(numbers)]
    if tag == "E":
        return ["E", 0 if exact else n, price, -0.0]
    return ["T", n, "long" if n % 2 else "short", 0, 60000,
            price, 110.0, 2.0, 20.0 if n % 2 else -5.0, 10.0,
            25.0, -2.0, -0.0, 0, 1, False, n, 1,
            'entry"漢', "exit", "audit\nonly"]


def projected_size(row):
    # Sizing only. All saved fragment bytes/digests come from strict_source.
    index, width = (0, 3) if row[0] == "E" else (1, 16)
    fields = review.existing.fixtures.header()["records"][index]["fields"]
    return len(review.compact(dict(zip(fields[:width], row[1:width + 1]))))


def exact_rows(tag):
    target = 6 * WINDOW
    rows, size = [], 0
    for n in range(1, 20001):
        row = values(tag, n, exact=True)
        added = projected_size(row) + int(bool(rows))
        if size + added > target:
            break
        rows.append(row)
        size += added
    else:
        raise AssertionError("exact-window population exceeded its explicit bound")
    assert len(rows) >= 32 and 0 <= target - size <= 32 * 15
    remaining = target - size
    time_index = 1 if tag == "E" else 3
    # Each of these 32 iterations adds 0..15 integer digits within i53.
    for index in range(len(rows) - 32, len(rows)):
        extra = min(15, remaining)
        rows[index][time_index] = 10 ** extra if extra else 0
        remaining -= extra
    assert remaining == 0
    assert sum(projected_size(row) for row in rows) + len(rows) - 1 == target
    return rows


def population(mode):
    equity = (exact_rows("E") if mode == "exact_equity"
              else [values("E", n) for n in range(1, E_ROWS + 1)])
    trades = (exact_rows("T") if mode == "exact_trades"
              else [values("T", n) for n in range(1, T_ROWS + 1)])
    assert equity and trades
    rows = []
    for pair in zip_longest(equity, trades):
        for row in pair:
            if row is not None:
                rows.append(review.row(row[0], *row[1:]))
    assert len(rows) == len(equity) + len(trades)
    if mode == "deferred_result":
        rows.append(review.row("E", 0, None, 0.0))
    return rows


def save(directory, payload, expected, *, bad_line=0):
    directory.mkdir()
    (directory / "source.sink").write_bytes(payload)
    (directory / "header.expected").write_bytes(expected.raw_header)
    for tag, name in (("E", "equity"), ("T", "trades")):
        (directory / f"{name}.expected").write_bytes(expected.fragments[tag])
    lines = payload.splitlines(keepends=True)
    offending = lines[bad_line - 1] if bad_line else b""
    (directory / "offending.expected").write_bytes(offending)
    offset = sum(map(len, lines[:bad_line - 1])) if bad_line else 0
    metadata = [
        f'{expected.counts["E"]} {expected.counts["T"]}',
        expected.digest,
        hashlib.sha256(expected.fragments["E"]).hexdigest(),
        hashlib.sha256(expected.fragments["T"]).hexdigest(),
        f"{bad_line} {offset}",
    ]
    (directory / "metadata.expected").write_text("\n".join(metadata) + "\n", encoding="ascii")


def fixture(root, mode):
    rows = population(mode)
    payload = review.pack(rows)
    expected = review.strict_source(payload)
    assert isinstance(expected, review.Source)
    assert all(len(expected.fragments[tag]) >= 5 * WINDOW for tag in ("E", "T"))
    if mode == "exact_equity":
        assert len(expected.fragments["E"]) == 6 * WINDOW
    elif mode == "exact_trades":
        assert len(expected.fragments["T"]) == 6 * WINDOW
    else:
        assert all(len(expected.fragments[tag]) % WINDOW for tag in ("E", "T"))
    bad_line = 0
    if mode == "source_refusal":
        def wrong_count(trailer):
            trailer["counts"]["E"] += 1
            return trailer
        payload = review.pack(rows, trailer_edit=wrong_count)
        assert isinstance(review.strict_source(payload), review.Refusal)
        bad_line = len(rows) + 2
    elif mode == "unsupported":
        # Depth exceeds the native parser's capacity. Strict field validation
        # also refuses it; neither route may consume the provisional fragments.
        nested = b'["E",' + b"[" * 20 + b"0" + b"]" * 20 + b",1.0,0.0]\n"
        payload = review.pack(rows + [("E", nested)])
        assert isinstance(review.strict_source(payload), review.Refusal)
        bad_line = len(rows) + 2
    directory = root / mode
    save(directory, payload, expected, bad_line=bad_line)
    if mode == "partial":
        empty = review.pack()
        empty_expected = review.strict_source(empty)
        assert isinstance(empty_expected, review.Source)
        assert empty_expected.fragments == {"E": b"", "T": b""}
        save(directory / "empty", empty, empty_expected)
        for tag, name in (("E", "equity_only"), ("T", "trades_only")):
            selected = [row for row in rows if row[0] == tag]
            assert selected
            single = review.pack(selected)
            single_expected = review.strict_source(single)
            assert isinstance(single_expected, review.Source)
            assert len(single_expected.fragments[tag]) >= 5 * WINDOW
            assert single_expected.fragments["T" if tag == "E" else "E"] == b""
            save(directory / name, single, single_expected)
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("mode", choices=MODES)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="sink-json-sync-") as temporary:
        directory = fixture(Path(temporary), arguments.mode)
        subprocess.run([str(arguments.executable), arguments.mode, str(directory)], check=True)


if __name__ == "__main__":
    main()
