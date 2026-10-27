"""CSV array/hash identity, previous-reader refusals, and bounded working memory."""
from contextlib import ExitStack
import csv
import ctypes
import errno
import gc
import hashlib
import io
from pathlib import Path
import random
import struct
import sys
import tracemalloc

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json


FIELDS = ("timestamp", "open", "high", "low", "close", "volume")
CELLS = ("1735776000000", "1", "2", "0.5", "1.5", "3")
PREFIX = b"pineforge:ohlcv:barc-le:v1\0"
BATCH = 4096
RECORD_BYTES = 48

# These helpers and the private function body below retain the previous
# implementation. The oracle never calls production's parsing/hash helpers.
BarC = run_json.BarC
_bars_unreadable = run_json._bars_unreadable
_SOURCE_FEED_RECORD = struct.Struct("<5dq")


def _new_source_feed_hasher():
    h = hashlib.sha256()
    h.update(PREFIX)
    return h


def _update_source_feed_hash(h, row):
    h.update(_SOURCE_FEED_RECORD.pack(*row))


def _previous_load_bars(csv_path: Path) -> tuple[ctypes.Array, int, str]:
    """Load the source tape once and return bars, count, and canonical hash.
    A file it cannot read is a ChartBarsError (a ValueError),
    chart_bars_unreadable{reason}: io (the file cannot be opened or read),
    columns (a row lacks a column), value (a value is not a number, or the file
    is not a UTF-8 CSV). An empty tape is returned as zero bars (main refuses
    it: reason empty)."""
    rows: list[tuple[float, float, float, float, float, int]] = []
    feed_hasher = _new_source_feed_hasher()
    try:
        with csv_path.open(newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    parsed = (
                        float(row["open"]),
                        float(row["high"]),
                        float(row["low"]),
                        float(row["close"]),
                        float(row["volume"]),
                        int(row["timestamp"]),
                    )
                except KeyError as e:
                    raise _bars_unreadable(
                        f"--ohlcv: {csv_path}: no column {e.args[0]}", "columns") from None
                except (TypeError, ValueError):
                    raise _bars_unreadable(
                        f"--ohlcv: {csv_path} line {reader.line_num}: not a number",
                        "value") from None
                _update_source_feed_hash(feed_hasher, parsed)
                rows.append(parsed)
    except OSError as e:
        raise _bars_unreadable(f"--ohlcv: {csv_path}: {e.strerror or e}", "io") from None
    except (UnicodeDecodeError, csv.Error) as e:
        raise _bars_unreadable(f"--ohlcv: {csv_path}: not a UTF-8 CSV ({e})", "value") from None
    n = len(rows)
    bars = (BarC * n)()
    for i, (o, h, l, c, v, ts) in enumerate(rows):
        bars[i].open      = o
        bars[i].high      = h
        bars[i].low       = l
        bars[i].close     = c
        bars[i].volume    = v
        bars[i].timestamp = ts
    return bars, n, feed_hasher.hexdigest()


def _csv_text(header=FIELDS, rows=(CELLS,), *, newline="\n", quoting=csv.QUOTE_MINIMAL):
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator=newline, quoting=quoting)
    writer.writerow(header)
    writer.writerows(rows)
    return output.getvalue()


def _write_synthetic(path, count, *, seed=937, header=FIELDS, newline="\n",
                     quoting=csv.QUOTE_MINIMAL, final_newline=True):
    """Own writer plus independent per-record native-byte and hash oracles."""
    assert count > 0
    rng = random.Random(seed)
    expected = bytearray()
    digest = hashlib.sha256(PREFIX)
    written = 0
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output, lineterminator=newline, quoting=quoting)
        writer.writerow(header)
        for index in range(count):
            price = rng.uniform(-5000, 5000)
            cells = {
                "timestamp": str(1735776000000 + index * 60000),
                "open": repr(price),
                "high": repr(price + rng.random()),
                "low": repr(price - rng.random()),
                "close": repr(price + rng.uniform(-1, 1)),
                "volume": repr(rng.uniform(0, 100000)),
                "note": 'ignored, "quoted"\r\n漢字\x85\u2028',
            }
            if index % 127 == 0:
                cells["volume"] = "-0"
            if index % 251 == 0:
                cells["open"] = "5e-324"
            writer.writerow([cells[name] for name in header])
            parsed = (float(cells["open"]), float(cells["high"]), float(cells["low"]),
                      float(cells["close"]), float(cells["volume"]),
                      int(cells["timestamp"]))
            expected.extend(struct.pack("@5dq", *parsed))
            digest.update(struct.pack("<5dq", *parsed))
            written += 1
    assert written == count
    if not final_newline:
        with path.open("r+b") as output:
            output.seek(-len(newline.encode("utf-8")), io.SEEK_END)
            output.truncate()
    return bytes(expected), digest.hexdigest()


def _assert_identity(path, count, expected=None):
    previous, old_count, old_hash = _previous_load_bars(path)
    actual, new_count, new_hash = run_json.load_bars(path)
    assert old_count == new_count == count
    assert len(previous) == len(actual) == count
    assert type(actual) is type(previous)
    assert ctypes.sizeof(actual) == RECORD_BYTES * count
    assert bytes(actual) == bytes(previous)
    assert new_hash == old_hash
    if expected is not None:
        assert (bytes(actual), new_hash) == expected
    return actual, new_hash


def _failure(loader, path, error_type):
    with pytest.raises(error_type) as caught:
        loader(path)
    error = caught.value
    assert type(error) is error_type
    return (type(error), str(error), getattr(error, "code", None),
            getattr(error, "code_args", None))


def _assert_refusal(path, *, reason, suffix=None, error_type=run_json.ChartBarsError):
    before = _failure(_previous_load_bars, path, error_type)
    after = _failure(run_json.load_bars, path, error_type)
    assert after == before
    if reason is not None:
        assert after[2:] == ("chart_bars_unreadable", {"reason": reason})
    if suffix is not None:
        assert after[1] == f"--ohlcv: {path}{suffix}"
    return after


def test_synthetic_array_and_hash_have_independent_reference(tmp_path):
    path = tmp_path / "synthetic.csv"
    count = 20003
    assert count >= 20000
    expected = _write_synthetic(path, count)
    bars, digest = _assert_identity(path, count, expected)
    assert len(bars) == count > 0
    assert digest == expected[1]


def test_fifty_random_csvs_match_previous_loader(tmp_path):
    rng = random.Random(30157)
    counts = [1, 4095, 4096, 4097, 8191, 8192, 8193]
    counts.extend(rng.randrange(1, 6000) for _ in range(43))
    assert len(counts) == 50
    assert {1, 4095, 4096, 4097}.issubset(counts)
    checked = 0
    for case, count in enumerate(counts):
        header = list(FIELDS)
        if case % 2:
            header.append("note")
        rng.shuffle(header)
        path = tmp_path / f"random-{case}.csv"
        expected = _write_synthetic(
            path, count, seed=rng.getrandbits(32), header=header,
            newline=rng.choice(("\n", "\r\n", "\r")),
            quoting=rng.choice((csv.QUOTE_MINIMAL, csv.QUOTE_ALL)),
            final_newline=bool(case % 3))
        bars, _ = _assert_identity(path, count, expected)
        assert len(bars) == count > 0
        checked += 1
    assert checked == 50


@pytest.mark.parametrize("newline", ("\n", "\r\n", "\r"), ids=("lf", "crlf", "cr"))
def test_partial_chunk_without_final_newline(tmp_path, newline):
    path = tmp_path / "partial.csv"
    count = 2 * BATCH + 37
    assert count % BATCH != 0
    expected = _write_synthetic(path, count, newline=newline, final_newline=False)
    with path.open("rb") as source:
        source.seek(-1, io.SEEK_END)
        assert source.read(1) not in (b"\n", b"\r")
    bars, _ = _assert_identity(path, count, expected)
    assert len(bars) == count > 0


@pytest.mark.parametrize("column", FIELDS)
def test_missing_header_column_has_previous_refusal(tmp_path, column):
    path = tmp_path / "missing-column.csv"
    header = [name for name in FIELDS if name != column]
    cells = dict(zip(FIELDS, CELLS))
    path.write_text(_csv_text(header, [[cells[name] for name in header]]), encoding="utf-8")
    _assert_refusal(path, reason="columns", suffix=f": no column {column}")


@pytest.mark.parametrize("column", FIELDS)
@pytest.mark.parametrize("cell", ("", "not-a-number", "1.5x"))
def test_invalid_numeric_cell_has_previous_refusal(tmp_path, column, cell):
    path = tmp_path / "invalid-cell.csv"
    row = list(CELLS)
    row[FIELDS.index(column)] = cell
    path.write_text(_csv_text(rows=[row]), encoding="utf-8")
    _assert_refusal(path, reason="value", suffix=" line 2: not a number")


@pytest.mark.parametrize("timestamp", ("1.5", "1e3", "nan", "inf", "-inf"))
def test_noninteger_timestamp_has_previous_refusal(tmp_path, timestamp):
    path = tmp_path / "timestamp.csv"
    path.write_text(_csv_text(rows=[(timestamp, *CELLS[1:])]), encoding="utf-8")
    _assert_refusal(path, reason="value", suffix=" line 2: not a number")


@pytest.mark.parametrize("width", range(1, len(FIELDS)))
def test_short_row_has_previous_refusal(tmp_path, width):
    path = tmp_path / "short.csv"
    path.write_text(_csv_text(rows=[CELLS[:width]]), encoding="utf-8")
    _assert_refusal(path, reason="value", suffix=" line 2: not a number")


@pytest.mark.parametrize("prefix,line", (("", 2), ("\n", 3), ("\n\n", 4),
                                       (",".join(CELLS) + "\n\n\n", 5)))
def test_blank_rows_keep_previous_error_line(tmp_path, prefix, line):
    path = tmp_path / "blank-lines.csv"
    bad = list(CELLS)
    bad[1] = "bad"
    path.write_text(",".join(FIELDS) + "\n" + prefix + ",".join(bad) + "\n",
                    encoding="utf-8")
    _assert_refusal(path, reason="value", suffix=f" line {line}: not a number")


def test_multiline_cell_keeps_previous_error_line(tmp_path):
    path = tmp_path / "multiline.csv"
    row = list(CELLS)
    row[1] = "\nnot-a-number"
    path.write_text(_csv_text(rows=[row], newline="\r\n"), encoding="utf-8")
    _assert_refusal(path, reason="value", suffix=" line 3: not a number")


@pytest.mark.parametrize("content,reason,suffix", (
    ("\ufeff" + _csv_text(), "columns", ": no column timestamp"),
    (_csv_text(("timestamp", " open", *FIELDS[2:])), "columns", ": no column open"),
    ("\n" + _csv_text(), "columns", ": no column open"),
    (_csv_text(FIELDS[:-1], [("1735776000000", "bad", "2", "0.5", "1.5")]),
     "value", " line 2: not a number"),
    (_csv_text((*FIELDS, "open"), [CELLS]), "value", " line 2: not a number"),
    (_csv_text((*FIELDS, "open"), [(*CELLS, "bad")]), "value", " line 2: not a number"),
))
def test_header_edge_refusals_match_previous_loader(tmp_path, content, reason, suffix):
    path = tmp_path / "header.csv"
    path.write_text(content, encoding="utf-8")
    _assert_refusal(path, reason=reason, suffix=suffix)


@pytest.mark.parametrize("prior", (0, 4095, 4096, 4097))
def test_late_numeric_refusal_matches_previous_loader(tmp_path, prior):
    path = tmp_path / "late-value.csv"
    path.write_text(_csv_text(rows=[CELLS] * prior + [(CELLS[0], "bad", *CELLS[2:])]),
                    encoding="utf-8")
    _assert_refusal(path, reason="value", suffix=f" line {prior + 2}: not a number")


@pytest.mark.parametrize("timestamp", (2**63, -2**63 - 1))
@pytest.mark.parametrize("prior", (0, 4095, 4096))
def test_timestamp_overflow_precedes_later_bad_row(tmp_path, timestamp, prior):
    path = tmp_path / "overflow.csv"
    rows = ([CELLS] * prior + [(timestamp, *CELLS[1:]),
                              (CELLS[0], "bad", *CELLS[2:])])
    path.write_text(_csv_text(rows=rows), encoding="utf-8")
    _assert_refusal(path, reason=None, error_type=struct.error)


@pytest.mark.parametrize("offset", (0, 8191, 8192, 8193, 256 * 1024 + 17))
def test_invalid_utf8_keeps_previous_decoder_diagnostic(tmp_path, offset):
    path = tmp_path / "not-utf8.csv"
    header = (",".join(FIELDS) + "\n").encode("utf-8")
    row = (",".join(CELLS) + "\n").encode("utf-8")
    if offset >= len(header):
        full, tail = divmod(offset - len(header), len(row))
        prefix = header + row * full + b" " * tail
    else:
        prefix = b" " * offset
    assert len(prefix) == offset
    path.write_bytes(prefix + b"\xff\n")
    result = _assert_refusal(path, reason="value")
    assert result[1].startswith(f"--ohlcv: {path}: not a UTF-8 CSV (")
    assert "decode" in result[1]


@pytest.mark.parametrize("later_failure", ("csv", "utf8"))
def test_first_numeric_refusal_precedes_later_parse_failure(tmp_path, later_failure):
    path = tmp_path / "first-failure.csv"
    first = _csv_text(rows=[(CELLS[0], "bad", *CELLS[2:])]).encode("utf-8")
    # Put the later failure beyond the decoder's first input block.
    padding = ((",".join(CELLS) + "\n") * 1000).encode("utf-8")
    last = b"\xff\n" if later_failure == "utf8" else b"x" * 200 + b"\n"
    path.write_bytes(first + padding + last)
    limit = csv.field_size_limit(128)
    try:
        _assert_refusal(path, reason="value", suffix=" line 2: not a number")
    finally:
        csv.field_size_limit(limit)


def test_csv_parser_refusal_keeps_previous_diagnostic(tmp_path):
    path = tmp_path / "large-field.csv"
    path.write_text(_csv_text(rows=[(CELLS[0], "x" * 200, *CELLS[2:])]), encoding="utf-8")
    limit = csv.field_size_limit(128)
    try:
        _assert_refusal(path, reason="value",
                        suffix=": not a UTF-8 CSV (field larger than field limit (128))")
    finally:
        csv.field_size_limit(limit)


def test_missing_file_keeps_previous_io_refusal(tmp_path):
    path = tmp_path / "missing.csv"
    _assert_refusal(path, reason="io")


@pytest.mark.parametrize("stage", ("open", "read", "close"))
@pytest.mark.parametrize("strerror", (None, "input failed"))
def test_io_refusals_keep_previous_text(stage, strerror):
    class BrokenInput:
        def __init__(self):
            self.lines = iter((_csv_text(rows=[]), ",".join(CELLS) + "\n"))

        def __str__(self):
            return "unreadable.csv"

        def fail(self):
            if strerror is None:
                raise OSError("input failed without strerror")
            raise OSError(errno.EIO, strerror)

        def open(self, **kwargs):
            if stage == "open":
                self.fail()
            return self

        def __enter__(self):
            return self

        def __exit__(self, *args):
            if stage == "close":
                self.fail()

        def __iter__(self):
            return self

        def __next__(self):
            line = next(self.lines, None)
            if line is not None:
                return line
            if stage == "read":
                self.fail()
            raise StopIteration

    before = _failure(_previous_load_bars, BrokenInput(), run_json.ChartBarsError)
    after = _failure(run_json.load_bars, BrokenInput(), run_json.ChartBarsError)
    text = strerror or "input failed without strerror"
    assert after == before == (run_json.ChartBarsError, f"--ohlcv: unreadable.csv: {text}",
                               "chart_bars_unreadable", {"reason": "io"})


@pytest.mark.parametrize("content", ("", "\n\n", _csv_text(rows=[]),
                                     "not,the,required,header\n"))
def test_empty_tape_and_caller_refusal_match_previous_loader(tmp_path, monkeypatch, content):
    path = tmp_path / "empty.csv"
    path.write_text(content, encoding="utf-8")
    bars, digest = _assert_identity(path, 0)
    assert len(bars) == 0
    assert digest == hashlib.sha256(PREFIX).hexdigest()
    current_loader = run_json.load_bars

    def no_strategy(*args, **kwargs):
        pytest.fail("an empty tape reached strategy loading")

    monkeypatch.setattr(run_json, "load_strategy", no_strategy)
    failures = []
    for loader in (_previous_load_bars, current_loader):
        monkeypatch.setattr(run_json, "load_bars", loader)
        with ExitStack() as resources, pytest.raises(run_json.ChartBarsError) as caught:
            run_json._main(["--so", "unused.so", "--ohlcv", str(path),
                            "--profile", "unjailed"], resources)
        error = caught.value
        failures.append((type(error), str(error), error.code, error.code_args))
    assert len(failures) == 2
    assert failures[0] == failures[1] == (
        run_json.ChartBarsError, f"--ohlcv: {path}: no bars",
        "chart_bars_unreadable", {"reason": "empty"})


@pytest.mark.parametrize("delta", (0, -60000), ids=("duplicate", "decreasing"))
def test_time_order_is_left_to_existing_callers(tmp_path, delta):
    path = tmp_path / "time-order.csv"
    first = int(CELLS[0])
    rows = [CELLS, (first + delta, *CELLS[1:]), (first - 2 * 60000, *CELLS[1:])]
    path.write_text(_csv_text(rows=rows), encoding="utf-8")
    bars, _ = _assert_identity(path, len(rows))
    assert len(bars) == 3
    assert [bar.timestamp for bar in bars] == [first, first + delta, first - 2 * 60000]


@pytest.mark.parametrize("cell", ("-0", "0", "5e-324", "-5e-324", "nan", "-nan",
                                  "inf", "-inf", "1e999", "-1e999", "-1"))
@pytest.mark.parametrize("column", FIELDS[1:])
def test_special_floats_keep_array_bits_and_hash(tmp_path, column, cell):
    path = tmp_path / "special.csv"
    row = list(CELLS)
    row[FIELDS.index(column)] = cell
    path.write_text(_csv_text(rows=[row]), encoding="utf-8")
    parsed = (*map(float, row[1:]), int(row[0]))
    expected = (struct.pack("@5dq", *parsed),
                hashlib.sha256(PREFIX + struct.pack("<5dq", *parsed)).hexdigest())
    bars, _ = _assert_identity(path, 1, expected)
    assert len(bars) == 1


@pytest.mark.parametrize("header,rows", (
    ((*FIELDS, "open"), [(*CELLS, "9"), (*CELLS, "-0")]),
    ((*FIELDS, "unused"), [CELLS, (*CELLS, "bad,ignored")]),
    (FIELDS, [(*CELLS, "extra", "bad"), CELLS]),
    (FIELDS, [("-9223372036854775808", *CELLS[1:]),
              ("9223372036854775807", *CELLS[1:])]),
    (FIELDS, [(CELLS[0], " \n1\t", *CELLS[2:])]),
))
def test_accepted_header_row_and_integer_edges(tmp_path, header, rows):
    path = tmp_path / "accepted.csv"
    path.write_text(_csv_text(header, rows), encoding="utf-8")
    bars, _ = _assert_identity(path, len(rows))
    assert len(bars) == len(rows) > 0


def test_hash_updates_are_bounded_record_batches(tmp_path, monkeypatch):
    path = tmp_path / "batches.csv"
    count = 5 * BATCH + 17
    expected = _write_synthetic(path, count)

    class HashProbe:
        def __init__(self):
            self.digest = hashlib.sha256(PREFIX)
            self.sizes = []

        def update(self, data):
            self.sizes.append(len(data))
            self.digest.update(data)

        def hexdigest(self):
            return self.digest.hexdigest()

    class RecordProbe:
        size = RECORD_BYTES

        def __init__(self):
            self.calls = 0

        def pack(self, *values):
            self.calls += 1
            return struct.pack("<5dq", *values)

    hasher = HashProbe()
    record = RecordProbe()
    monkeypatch.setattr(run_json, "_new_source_feed_hasher", lambda: hasher)
    monkeypatch.setattr(run_json, "_SOURCE_FEED_RECORD", record)
    bars, actual_count, digest = run_json.load_bars(path)
    assert actual_count == len(bars) == count > 0
    assert (bytes(bars), digest) == expected
    full, tail = divmod(count, BATCH)
    assert tail > 0
    assert hasher.sizes == [BATCH * RECORD_BYTES] * full + [tail * RECORD_BYTES]
    assert sum(hasher.sizes) == count * RECORD_BYTES
    assert record.calls == 0


def test_transient_memory_is_bounded_beyond_returned_array(tmp_path):
    checked = 0
    for count in (8193, 65539):
        path = tmp_path / f"memory-{count}.csv"
        _write_synthetic(path, count)
        gc.collect()
        tracemalloc.start()
        try:
            bars, actual_count, digest = run_json.load_bars(path)
            retained, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        assert actual_count == len(bars) == count > 0
        assert ctypes.sizeof(bars) == count * RECORD_BYTES
        assert len(digest) == 64
        # Exclude the necessary returned array/backing. This fixed allowance
        # covers one numeric batch, pack arguments/output, and parser buffers.
        # Keeping every row's float/int objects exceeds it on the larger tape.
        assert peak - retained <= 4 * 1024 * 1024
        del bars
        checked += 1
    assert checked == 2


def test_returned_array_owns_backing_and_remains_writable(tmp_path):
    path = tmp_path / "ownership.csv"
    count = BATCH + 1
    expected, expected_hash = _write_synthetic(path, count)
    bars, actual_count, digest = run_json.load_bars(path)
    assert actual_count == len(bars) == count > 0
    gc.collect()
    assert (bytes(bars), digest) == (expected, expected_hash)
    changed = bytearray(expected)
    bars[count - 1].close = -0.0
    struct.pack_into("@d", changed, (count - 1) * RECORD_BYTES + 24, -0.0)
    bars[0].timestamp = -(2**63)
    struct.pack_into("@q", changed, 40, -(2**63))
    assert bytes(bars) == changed
