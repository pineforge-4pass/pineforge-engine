import base64
import contextlib
import ctypes
import io
import json
import math
import sys
from pathlib import Path

import pytest
import run_json  # docker/ is on sys.path in the engine test env


# --symbol-feeds: other symbols' bars for request.security, keyed by the exact
# symbol string and timeframe, installed through strategy_set_symbol_facts and
# strategy_set_symbol_feed before the run; absent changes nothing.

CORE = ("pf_abi_version", "strategy_create", "strategy_set_input", "strategy_set_override",
        "run_backtest_full", "strategy_free", "report_free")
FEED_SETTERS = ("strategy_set_symbol_facts", "strategy_set_symbol_feed")
ST = 7
H4 = 4 * 3600 * 1000
DAY = 24 * 3600 * 1000
T0 = 1759536000000  # 2025-10-04T00:00:00Z
ETH = {"ticker": "ETHUSDT", "tickerid": "BINANCE:ETHUSDT", "type": "crypto",
       "currency": "USDT", "basecurrency": "ETH", "mintick": 0.01, "pointvalue": 1.0,
       "mincontract": 0.0001, "session": "24x7", "timezone": "UTC", "country": None}
ETH_FACTS = [(b"canonical", b"BINANCE:ETHUSDT"), (b"type", b"crypto"), (b"timezone", b"UTC"),
             (b"session", b"24x7"), (b"currency", b"USDT"), (b"mintick", b"0.01")]


class FakeLib:
    """A strategy library stand-in: each named symbol records (name, *args) and
    returns its configured result; a name left out fails hasattr like a missing
    export."""

    def __init__(self, names, returns=None):
        self.calls = []
        for name in names:
            setattr(self, name, self._fn(name, (returns or {}).get(name)))

    def _fn(self, name, result):
        def call(*args):
            self.calls.append((name,) + args)
            return result
        return call


def fake_lib(setters=FEED_SETTERS, returns=None):
    # strategy_create returns a handle: run_json refuses a NULL one before any setter.
    rets = {"pf_abi_version": run_json.EXPECTED_PF_ABI, "strategy_create": ST,
            **{name: 0 for name in FEED_SETTERS}, **(returns or {})}
    return FakeLib(CORE + tuple(setters), rets)


def csv_text(rows, header="timestamp,open,high,low,close,volume"):
    return header + "\n" + "".join(",".join(str(v) for v in row) + "\n" for row in rows)


def bars(start, step, count, base=100.0):
    return [(start + i * step, base + i, base + i + 2, base + i - 1, base + i + 1, 10 + i)
            for i in range(count)]


def index(tmp_path, symbols, files=None, name="symbols.json"):
    for fname, text in (files or {}).items():
        (tmp_path / fname).write_text(text)
    p = tmp_path / name
    p.write_text(symbols if isinstance(symbols, str) else json.dumps({"symbols": symbols}))
    return p


def eth_index(tmp_path, syminfo=ETH):
    entry = {"feeds": {"240": "eth-240.csv", "D": "eth-1D.csv"}}
    if syminfo is not None:
        entry["syminfo"] = syminfo
    return index(tmp_path, {"BINANCE:ETHUSDT": entry},
                 {"eth-240.csv": csv_text(bars(T0, H4, 3)),
                  "eth-1D.csv": csv_text(bars(T0 - DAY, DAY, 2, base=200.0))})


def feed_calls(lib):
    out = []
    for call in lib.calls:
        if call[0] == "strategy_set_symbol_feed":
            _, st, key, tf, arr, close, n = call
            out.append((st, key, tf, [(arr[i].timestamp, arr[i].open, arr[i].high,
                                       arr[i].low, arr[i].close, arr[i].volume)
                                      for i in range(n)], list(close[:n]), n))
    return out


def test_facts_then_feeds_keyed_by_the_exact_string_and_canonical_timeframe(tmp_path):
    lib = fake_lib()
    symbols = run_json.load_symbol_feeds(eth_index(tmp_path))
    run_json.install_symbol_feeds(lib, ST, symbols)
    facts = [c for c in lib.calls if c[0] == "strategy_set_symbol_facts"]
    assert facts == [("strategy_set_symbol_facts", ST, b"BINANCE:ETHUSDT", f, v)
                     for f, v in ETH_FACTS]
    assert [c[0] for c in lib.calls] == ["strategy_set_symbol_facts"] * 6 \
        + ["strategy_set_symbol_feed"] * 2
    (_, k1, tf1, rows1, close1, n1), (_, k2, tf2, rows2, close2, n2) = feed_calls(lib)
    assert (k1, tf1, n1, k2, tf2, n2) == (b"BINANCE:ETHUSDT", b"240", 3,
                                          b"BINANCE:ETHUSDT", b"1D", 2)
    assert rows1[0] == (T0, 100.0, 102.0, 99.0, 101.0, 10.0)
    # No time_close column: each close is the open plus the timeframe.
    assert close1 == [T0 + H4, T0 + 2 * H4, T0 + 3 * H4]
    assert close2 == [T0, T0 + DAY]


def test_record_names_what_was_installed(tmp_path):
    rec = run_json.symbol_feeds_record(run_json.load_symbol_feeds(eth_index(tmp_path)))
    eth = rec["symbols"]["BINANCE:ETHUSDT"]
    assert rec["canonicalization"] == "pf-symbol-feed-barc-close-le-v1"
    assert eth["facts"] == {"canonical": "BINANCE:ETHUSDT", "type": "crypto",
                            "timezone": "UTC", "session": "24x7", "currency": "USDT",
                            "mintick": 0.01}
    assert sorted(eth["feeds"]) == ["1D", "240"]
    assert eth["feeds"]["240"]["bars"] == 3
    assert (eth["feeds"]["240"]["first_ts"], eth["feeds"]["240"]["last_ts"]) == (T0, T0 + 2 * H4)
    assert len(eth["feeds"]["240"]["source_values_sha256"]) == 64


def test_without_syminfo_only_feeds_are_installed(tmp_path):
    lib = fake_lib()
    run_json.install_symbol_feeds(lib, ST, run_json.load_symbol_feeds(eth_index(tmp_path, None)))
    assert [c[0] for c in lib.calls] == ["strategy_set_symbol_feed"] * 2


def test_a_wrapped_syminfo_is_read_like_a_flat_one(tmp_path):
    flat = run_json.load_symbol_feeds(eth_index(tmp_path))[0]["facts"]
    wrapped = run_json.load_symbol_feeds(eth_index(tmp_path, {"syminfo": ETH}))[0]["facts"]
    assert flat == wrapped


def test_time_close_column_gives_each_close(tmp_path):
    rows = [(T0, 1, 2, 0.5, 1.5, "", T0 + 6 * 3600 * 1000),
            (T0 + DAY, 1, 2, 0.5, 1.5, "NaN", T0 + DAY + 6 * 3600 * 1000)]
    p = index(tmp_path, {"NASDAQ:QQQ": {"feeds": {"1D": "qqq.csv"}}},
              {"qqq.csv": csv_text(rows, "timestamp,open,high,low,close,volume,time_close")})
    lib = fake_lib()
    run_json.install_symbol_feeds(lib, ST, run_json.load_symbol_feeds(p))
    (_, key, tf, got, close, n), = feed_calls(lib)
    assert (key, tf, n) == (b"NASDAQ:QQQ", b"1D", 2)
    assert close == [T0 + 6 * 3600 * 1000, T0 + DAY + 6 * 3600 * 1000]
    assert all(math.isnan(r[5]) for r in got)  # empty or NaN volume: none published


@pytest.mark.parametrize("tf,opens,closes", [
    ("1M", [1767225600000, 1769904000000], [1769904000000, 1772323200000]),  # Jan, Feb 2026
    ("1W", [T0], [T0 + 7 * DAY]),
    ("15", [T0], [T0 + 15 * 60 * 1000]),
    ("30S", [T0], [T0 + 30 * 1000]),
])
def test_derived_close_per_timeframe(tmp_path, tf, opens, closes):
    rows = [(t, 1, 2, 0.5, 1.5, 3) for t in opens]
    p = index(tmp_path, {"X:Y": {"feeds": {tf: "f.csv"}}}, {"f.csv": csv_text(rows)})
    lib = fake_lib()
    run_json.install_symbol_feeds(lib, ST, run_json.load_symbol_feeds(p))
    assert feed_calls(lib)[0][4] == closes


def test_prefix_and_suffix_are_part_of_the_key(tmp_path):
    feed = csv_text(bars(T0, H4, 2))
    p = index(tmp_path, {s: {"feeds": {"240": "f.csv"}}
                         for s in ("ETHUSDT", "BINANCE:ETHUSDT", "BINANCE:ETHUSDT.P")},
              {"f.csv": feed})
    lib = fake_lib()
    run_json.install_symbol_feeds(lib, ST, run_json.load_symbol_feeds(p))
    assert [c[1] for c in feed_calls(lib)] == [b"ETHUSDT", b"BINANCE:ETHUSDT",
                                               b"BINANCE:ETHUSDT.P"]


GOOD = csv_text(bars(T0, H4, 2))


@pytest.mark.parametrize("symbols,files,message", [
    ({"E": {"feeds": {"4h": "f.csv"}}}, {"f.csv": GOOD}, 'a timeframe is whole minutes'),
    ({"E": {"feeds": {"60m": "f.csv"}}}, {"f.csv": GOOD}, 'got "60m"'),
    ({"E": {"feeds": {"D": "f.csv", "1D": "f.csv"}}}, {"f.csv": GOOD}, "two feeds at timeframe 1D"),
    ({"E": {"feed": {"240": "f.csv"}}}, {"f.csv": GOOD}, "an entry is"),
    ({"E": {"feeds": ["f.csv"]}}, {"f.csv": GOOD}, "feeds must be an object"),
    ({"": {"feeds": {"240": "f.csv"}}}, {"f.csv": GOOD}, "a symbol must be a non-empty string"),
    ({"E\n": {"feeds": {"240": "f.csv"}}}, {"f.csv": GOOD}, "without control characters"),
    ({"E": {"feeds": {"240": ""}}}, {}, "the feed must name a CSV file"),
    ({"E": {"feeds": {"240": "nope.csv"}}}, {}, "nope.csv"),
    ({"E": {"feeds": {"240": "f.csv"}}}, {"f.csv": "timestamp,open,high,low\n1,2,3,4\n"},
     "no column close"),
    ({"E": {"feeds": {"240": "f.csv"}}}, {"f.csv": csv_text([(T0, "x", 1, 1, 1, 1)])},
     "line 2: not a number"),
    ({"E": {"feeds": {"240": "f.csv"}}}, {"f.csv": csv_text([(T0, "inf", 1, 1, 1, 1)])},
     "line 2: prices must be finite"),
    ({"E": {"feeds": {"240": "f.csv"}}}, {"f.csv": csv_text([(T0, 1, 1, 1, 1, -5)])},
     "volume nonnegative"),
    ({"E": {"feeds": {"240": "f.csv"}}},
     {"f.csv": csv_text([(T0 + H4, 1, 1, 1, 1, 1), (T0, 1, 1, 1, 1, 1)])},
     "line 3: timestamps must increase"),
    # 4h bars declared as 8h: each close lands after the next bar's open.
    ({"E": {"feeds": {"480": "f.csv"}}}, {"f.csv": GOOD}, "is the timeframe right?"),
    ({"E": {"syminfo": {"mintick": 0}, "feeds": {"240": "f.csv"}}}, {"f.csv": GOOD},
     "syminfo.mintick must be a positive finite number, got 0"),
    ({"E": {"syminfo": {"mintick": "0.01"}, "feeds": {"240": "f.csv"}}}, {"f.csv": GOOD},
     'got "0.01"'),
    ({"E": {"syminfo": [], "feeds": {"240": "f.csv"}}}, {"f.csv": GOOD},
     "syminfo must be an object"),
    ({"E": {"syminfo": {"session": 5}, "feeds": {"240": "f.csv"}}}, {"f.csv": GOOD},
     "syminfo.session must be a non-empty string"),
], ids=["4h", "60m", "D-and-1D", "unknown-entry-key", "feeds-list", "empty-symbol",
        "control-char", "empty-path", "missing-file", "missing-column", "not-a-number",
        "infinite-price", "negative-volume", "decreasing", "wrong-timeframe", "mintick-zero",
        "mintick-string", "syminfo-list", "session-number"])
def test_bad_index_or_feed_is_refused_by_name(tmp_path, symbols, files, message):
    with pytest.raises(run_json.SymbolFeedsError, match=None) as e:
        run_json.load_symbol_feeds(index(tmp_path, symbols, files))
    assert message in str(e.value)
    assert str(e.value).startswith("--symbol-feeds")


@pytest.mark.parametrize("text,message", [
    ("[]", 'the index must be {"symbols": {...}}'),
    ('{"symbol": {}}', 'the index must be {"symbols": {...}}'),
    ("{not json", "is not JSON"),
    ('{"symbols": {"E": {"feeds": {}}, "E": {"feeds": {}}}}', 'duplicate key "E"'),
], ids=["list", "no-symbols", "not-json", "duplicate-key"])
def test_bad_index_document_is_refused(tmp_path, text, message):
    with pytest.raises(run_json.SymbolFeedsError) as e:
        run_json.load_symbol_feeds(index(tmp_path, text))
    assert message in str(e.value)


def test_more_than_256_feeds_is_refused(tmp_path):
    symbols = {f"S{i}": {"feeds": {"240": "f.csv"}} for i in range(257)}
    with pytest.raises(run_json.SymbolFeedsError, match="more than 256"):
        run_json.load_symbol_feeds(index(tmp_path, symbols, {"f.csv": GOOD}))


@pytest.mark.parametrize("setters", [FEED_SETTERS[:1], FEED_SETTERS[1:], ()],
                         ids=["no-feed", "no-facts", "neither"])
def test_a_library_without_the_setters_is_refused(tmp_path, setters):
    lib = fake_lib(setters)
    with pytest.raises(run_json.SymbolFeedsError, match="cannot be installed"):
        run_json.install_symbol_feeds(lib, ST, run_json.load_symbol_feeds(eth_index(tmp_path)))
    assert not [c for c in lib.calls if c[0] in FEED_SETTERS]


def test_an_engine_refusal_names_the_feed_and_the_engine_error(tmp_path):
    lib = fake_lib(FEED_SETTERS + ("strategy_get_last_error",),
                   {"strategy_set_symbol_feed": -1, "strategy_get_last_error": b"bars must increase"})
    with pytest.raises(run_json.SymbolFeedsError) as e:
        run_json.install_symbol_feeds(lib, ST, run_json.load_symbol_feeds(eth_index(tmp_path)))
    assert str(e.value) == ("--symbol-feeds: the engine refused the feed BINANCE:ETHUSDT@240: "
                            "bars must increase")


def test_load_strategy_declares_the_setter_signatures(monkeypatch):
    monkeypatch.setattr(ctypes, "CDLL", lambda path: fake_lib())
    lib = run_json.load_strategy(Path("fake.so"))
    assert lib.strategy_set_symbol_feed.argtypes == [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(run_json.BarC),
        ctypes.POINTER(ctypes.c_int64), ctypes.c_int]
    assert lib.strategy_set_symbol_feed.restype is ctypes.c_int
    assert lib.strategy_set_symbol_facts.argtypes == [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]


# main() against the fake library.

@pytest.fixture
def harness(tmp_path, monkeypatch):
    tape = tmp_path / "tape.csv"
    tape.write_text("open,high,low,close,volume,timestamp\n"
                    "1,2,0.5,1.5,10,1000\n1.5,3,1,2.5,20,2000\n")

    def run(lib, *extra):
        argv = ["run_json.py", "--so", "fake.so", "--ohlcv", str(tape), *extra]
        monkeypatch.setattr(ctypes, "CDLL", lambda path: lib)
        monkeypatch.setattr(sys, "argv", argv)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = run_json.main()
        return status, out.getvalue()

    return run


def report_of(text):
    rep = json.loads(text)
    rep["elapsed_seconds"] = 0
    return rep


def count(lib, name):
    return [c[0] for c in lib.calls].count(name)


def test_main_without_feeds_or_with_an_empty_index_reports_as_before(harness, tmp_path):
    _, plain = harness(fake_lib())
    status, out = harness(fake_lib(), "--symbol-feeds", str(index(tmp_path, {})))
    assert status == 0
    assert report_of(out) == report_of(plain)
    assert "symbol_feeds" not in report_of(out)["applied_runtime"]


def test_main_installs_feeds_and_records_them_in_the_fingerprint(harness, tmp_path):
    _, plain = harness(fake_lib())
    lib = fake_lib()
    status, out = harness(lib, "--symbol-feeds", str(eth_index(tmp_path)))
    rep = report_of(out)
    token = base64.b64decode(rep["fingerprint"]["token"])
    assert status == 0
    assert count(lib, "strategy_set_symbol_feed") == 2
    # Installed on the state before its run.
    names = [c[0] for c in lib.calls]
    assert names.index("strategy_set_symbol_feed") < names.index("run_backtest_full")
    recorded = rep["applied_runtime"]["symbol_feeds"]
    assert sorted(recorded["symbols"]["BINANCE:ETHUSDT"]["feeds"]) == ["1D", "240"]
    assert json.loads(token)["runtime"]["symbol_feeds"] == recorded
    assert rep["fingerprint"]["digest"] != report_of(plain)["fingerprint"]["digest"]


def test_main_bench_installs_feeds_on_every_state(harness, tmp_path):
    lib = fake_lib()
    status, _ = harness(lib, "--bench", "--warmup", "2", "--repeats", "3",
                        "--symbol-feeds", str(eth_index(tmp_path)))
    states = 2 + 3 + 1
    assert status == 0
    assert count(lib, "strategy_create") == count(lib, "strategy_free") == states
    assert count(lib, "strategy_set_symbol_feed") == 2 * states


def test_main_refuses_a_bad_index_with_one_structured_line_before_any_state(harness, tmp_path):
    lib = fake_lib()
    bad = index(tmp_path, {"E": {"feeds": {"4h": "f.csv"}}}, {"f.csv": GOOD})
    status, out = harness(lib, "--symbol-feeds", str(bad))
    assert status == 1
    assert out.endswith("\n") and out.count("\n") == 1
    assert json.loads(out) == {"engine": "pineforge", "error":
                               '--symbol-feeds: a timeframe is whole minutes ("15", "240") '
                               'or <n>D|W|M|S ("1D", "1W"), got "4h"',
                               "code": "symbol_feeds_refused",
                               "args": {"reason": "timeframe_invalid"}}
    assert count(lib, "strategy_create") == count(lib, "run_backtest_full") == 0


def test_main_without_the_setters_fails_and_frees_the_state(harness, tmp_path):
    lib = fake_lib(())
    status, out = harness(lib, "--symbol-feeds", str(eth_index(tmp_path)))
    assert status == 1
    assert json.loads(out)["error"].startswith("--symbol-feeds: the strategy library has no "
                                               "strategy_set_symbol_facts, strategy_set_symbol_feed")
    assert (json.loads(out)["code"], json.loads(out)["args"]) == (
        "symbol_feeds_refused", {"reason": "library_without_symbol_feeds"})
    assert count(lib, "strategy_create") == count(lib, "strategy_free") == 1
    assert count(lib, "run_backtest_full") == 0


def test_a_header_only_feed_is_installed_without_bars(tmp_path):
    # The engine's contract: a feed without bars, whose requests read na.
    p = index(tmp_path, {"X:Y": {"feeds": {"60": "f.csv"}}}, {"f.csv": csv_text([])})
    lib = fake_lib()
    symbols = run_json.load_symbol_feeds(p)
    run_json.install_symbol_feeds(lib, ST, symbols)
    assert [(c[2], c[3], c[6]) for c in lib.calls if c[0] == "strategy_set_symbol_feed"] \
        == [(b"X:Y", b"60", 0)]
    record = run_json.symbol_feeds_record(symbols)["symbols"]["X:Y"]["feeds"]["60"]
    assert record["bars"] == 0 and "first_ts" not in record


def test_an_empty_time_close_cell_falls_back_to_open_plus_timeframe(tmp_path):
    rows = [(T0, 1, 2, 0.5, 1.5, 3, ""), (T0 + H4, 1, 2, 0.5, 1.5, 3, T0 + H4 + 3600 * 1000)]
    p = index(tmp_path, {"X:Y": {"feeds": {"240": "f.csv"}}},
              {"f.csv": csv_text(rows, "timestamp,open,high,low,close,volume,time_close")})
    lib = fake_lib()
    run_json.install_symbol_feeds(lib, ST, run_json.load_symbol_feeds(p))
    assert feed_calls(lib)[0][4] == [T0 + H4, T0 + H4 + 3600 * 1000]


@pytest.mark.parametrize("opens,closes", [
    ([1764547200000], [1767225600000]),  # 2025-12-01 -> 2026-01-01
    ([1769817600000], [1772236800000]),  # 2026-01-31 -> 2026-02-28 (day clamped)
    ([1767225600123], [1769904000123]),  # milliseconds kept
], ids=["dec-jan", "clamp", "ms"])
def test_month_close_edges(tmp_path, opens, closes):
    p = index(tmp_path, {"X:Y": {"feeds": {"1M": "f.csv"}}},
              {"f.csv": csv_text([(t, 1, 2, 0.5, 1.5, 3) for t in opens])})
    lib = fake_lib()
    run_json.install_symbol_feeds(lib, ST, run_json.load_symbol_feeds(p))
    assert feed_calls(lib)[0][4] == closes


def test_a_utf8_bom_is_read(tmp_path):
    p = index(tmp_path, {"X:Y": {"feeds": {"240": "f.csv"}}})
    (tmp_path / "f.csv").write_bytes(b"\xef\xbb\xbf" + GOOD.encode())
    assert run_json.load_symbol_feeds(p)[0]["feeds"][0]["n"] == 2


def test_line_numbers_count_blank_lines(tmp_path):
    text = csv_text([(T0, 1, 2, 0.5, 1.5, 3)]) + "\n" + f"{T0 + H4},x,2,0.5,1.5,3\n"
    with pytest.raises(run_json.SymbolFeedsError, match="line 4: not a number"):
        run_json.load_symbol_feeds(index(tmp_path, {"X:Y": {"feeds": {"240": "f.csv"}}},
                                         {"f.csv": text}))


@pytest.mark.parametrize("tf,make,message", [
    ("240", lambda p: p.write_bytes(b"timestamp,open,high,low,close,volume\n1,caf\xe9,1,1,1,1\n"),
     "not a UTF-8 CSV"),
    ("240", lambda p: p.write_text(csv_text([(2**53, 1, 1, 1, 1, 1)])), "within +-9007199254740991"),
    # Microsecond stamps read as milliseconds: a monthly close beyond the calendar.
    ("1M", lambda p: p.write_text(csv_text([(1759536000000000, 1, 1, 1, 1, 1)])), "within"),
], ids=["cp1252", "beyond-2^53", "microseconds-month"])
def test_undecodable_or_out_of_range_feeds_are_refused(tmp_path, tf, make, message):
    make(tmp_path / "f.csv")
    with pytest.raises(run_json.SymbolFeedsError) as e:
        run_json.load_symbol_feeds(index(tmp_path, {"X:Y": {"feeds": {tf: "f.csv"}}}))
    assert message in str(e.value)


@pytest.mark.parametrize("raw,message", [
    ('{"symbols": {"\\ud800": {"feeds": {}}}}', "a symbol must be a non-empty string"),
    ('{"symbols": {"E": {"syminfo": {"mintick": 1' + "0" * 400 + '}, "feeds": {}}}}',
     "syminfo.mintick must be a positive finite number"),
    ("[" * 100000 + "]" * 100000, "is not JSON"),
], ids=["lone-surrogate", "huge-mintick", "deep-nesting"])
def test_index_values_that_would_raise_are_refused(tmp_path, raw, message):
    with pytest.raises(run_json.SymbolFeedsError) as e:
        run_json.load_symbol_feeds(index(tmp_path, raw))
    assert message in str(e.value)
