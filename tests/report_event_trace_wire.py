"""Independent v1 byte oracle: only the fixed schema and script input formula."""
import hashlib
import json
import pathlib
import sys

START = 1704067200000
STEP = 60000

# The v1 wire schema, including descriptors for types with zero records.
COLUMNS = {
    "E": (
        ("time_ms", "i64", "unix_ms"),
        ("equity", "f64?", "account_ccy"),
        ("open_profit", "f64?", "account_ccy"),
    ),
    "T": (
        ("n", "i64", ""), ("side", "str", ""),
        ("entry_time", "i64", "unix_ms"), ("exit_time", "i64", "unix_ms"),
        ("entry_price", "f64?", "quote_ccy"), ("exit_price", "f64?", "quote_ccy"),
        ("qty", "f64?", "qty"), ("pnl", "f64?", "account_ccy"),
        ("pnl_pct", "f64?", "percent"), ("max_runup", "f64?", "account_ccy"),
        ("max_drawdown", "f64?", "account_ccy"), ("commission", "f64?", "account_ccy"),
        ("entry_bar_index", "i64", "script_bar"), ("exit_bar_index", "i64", "script_bar"),
        ("open_at_end", "bool", ""), ("entry_incarnation", "u64", ""),
        ("close_cause", "i64", ""), ("entry_id", "str", ""),
        ("exit_id", "str", ""), ("exit_comment", "str", ""),
    ),
    "O": (("row", "i64", ""), ("open_ms", "i64?", "unix_ms"),
          ("close_ms", "i64?", "unix_ms"), ("v0", "f64?", "")),
    "V": (
        ("sequence", "u64", ""), ("output_index", "i64", ""), ("bar_index", "i64", ""),
        ("bar_open_ms", "i64?", "unix_ms"), ("bar_close_ms", "i64?", "unix_ms"),
        ("ordinal_in_bar", "i64", ""), ("phase", "i64", ""), ("confirmed", "i64", ""),
        ("value", "f64?", ""), ("message_hash64", "hex64", ""), ("message", "str?", ""),
    ),
    "K": (("c0", "f64?", ""),),
    "X": (("timestamp", "i64", "unix_ms"), ("bar_index", "i64", ""),
          ("name", "str", ""), ("value", "f64?", "")),
}


def quote(text):
    # Sink v1 uses \u00xx for every control byte, including LF and NUL.
    return '"' + "".join(
        f"\\u{ord(char):04x}" if ord(char) < 32
        else "\\" + char if char in '"\\' else char for char in text
    ) + '"'


def line(value):
    if value is None:
        return "null"
    if isinstance(value, str):
        return quote(value)
    if isinstance(value, list):
        return "[" + ",".join(map(line, value)) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(quote(key) + ":" + line(item) for key, item in value.items()) + "}"
    return json.dumps(value, allow_nan=False, separators=(",", ":"))


def fnv(text):
    result = 14695981039346656037
    for byte in text.encode():
        result = ((result ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{result:016x}"


def oracle(version, count, recalculate, clear):
    assert count > 0
    descriptors = [
        dict(type=tag, fields=[c[0] for c in columns],
             types=[c[1] for c in columns], units=[c[2] for c in columns])
        for tag, columns in COLUMNS.items()
    ]
    records = [dict(schema="pf-report-sink", version=1, run_id="event-trace",
                    engine=version, records=descriptors)]

    def output(index):
        opened = START + index * STEP
        closed = (None if index % 2 == 0 else opened + STEP + 1) if recalculate else opened + STEP
        value = [-0.0, None, 1.25][index % 3]
        message = (None if index % 3 == 0 else "" if index % 3 == 1
                   else 'event/"\\\n\0' + str(index) + "m" * 160)
        return ["O", index, opened, closed, value], [
            "V", index + 1, 0, index, opened, closed, 0, 0, 1, value,
            "0000000000000000" if message is None else fnv(message), message,
        ]

    for index in range(count):
        if index:
            row, event = output(index - 1)
            records.append(row)
            if not clear:
                records.append(event)
        if clear:
            records.append(output(index)[1])
        records.append(["X", START + (index + 1) * STEP, index,
                        'trace/"\n\0' + str(index) + "n" * 48,
                        [-0.0, None, 1.25][index % 3]])
        # The next completed calculation retires the previous held E point,
        # after that calculation's callback and its X record.
        if index:
            records.append(["E", START + (index - 1) * STEP, 10000.0, 0.0])
    records.append(["E", START + (count - 1) * STEP, 10000.0, 0.0])
    records.append(output(count - 1)[0])
    if not clear:
        records.append(output(count - 1)[1])
    records.append(["K", -0.0])
    prefix = "".join(line(record) + "\n" for record in records).encode()
    digest = hashlib.sha256(prefix).hexdigest()
    trailer = dict(trailer="pf-report-sink", version=1,
                   counts=dict(E=count, T=0, O=count, V=count, K=1, X=count), sha256=digest)
    return prefix + (line(trailer) + "\n").encode(), digest


if __name__ == "__main__":
    artifact, version, count, recalculate, clear, receipt_sha = sys.argv[1:]
    expected_bytes, expected_sha = oracle(version, int(count), recalculate == "1", clear == "1")
    actual = pathlib.Path(artifact).read_bytes()
    assert actual == expected_bytes, "complete sink bytes differ from independent v1 oracle"
    assert receipt_sha == expected_sha, "receipt SHA differs from independent hashlib digest"
