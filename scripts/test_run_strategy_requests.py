#!/usr/bin/env python3
"""The harness side of lane XSYM-D: a probe's pinned request data.

The case runner sets PINEFORGE_REQUESTS_ROOT=<dir> holding
<dir>/<slug>/requests.json (pineforge-probe-requests/v1) and
<dir>/<slug>/files/<sha256> (workflow docs/xsym-requests.md, "The environment
contract"); run_strategy.py finds its probe as the basename of the strategy
directory it is handed. These rows pin that contract on synthetic data:

  * unset, or no manifest for the probe: nothing is read, and main() hands the
    engine exactly the arguments it always did;
  * a manifest is held to its schema and to this probe, every file it names is
    verified against its sha256 (a feed against its byte count too), and a
    malformed one is refused by name, before any engine call;
  * only the probe's own manifest and files are read: a sibling slug's broken
    manifest and a case-wide PINEFORGE_PINE_LIBRARIES are never opened;
  * the facts, feeds, columns and recorded series are installed through the
    four symbol-data setters, before the run, and the run provenance records
    each (key, sha, bars);
  * a library missing the setters is refused, and so is the docker runner.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_strategy as rs
from run_strategy import (
    REQUESTS_ROOT_ENV,
    RequestsManifestError,
    Strategy,
    build_runtime_provenance,
    load_probe_requests,
    probe_requests_provenance,
    validate_probe_requests,
)

SLUG = "xsymd-harness-probe"
FEED_CSV = (
    "timestamp,time_close,open,high,low,close,volume,fp_delta_100_70\n"
    "1743984000000,1743984900000,102.647,102.859,102.607,102.854,,12\n"
    "1743984900000,1743985800000,102.853,102.871,102.741,102.792,NaN,\n"
    "1743985800000,1743986700000,102.795,102.854,102.74,102.848,5,-3.5\n"
).encode()
TAPE_CSV = b"chart_open_ms,value\n1746192600000,1.65\n1754055000000,1.57\n"
LIBRARY = b"//@version=6\nlibrary(\"xsymd\")\nexport f() => 1\n"
FACTS_SHA = "a" * 64
PROV_SHA = "b" * 64


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _manifest(**overrides) -> dict:
    doc = {
        "schemaVersion": "pineforge-probe-requests/v1",
        "probe": {"probeId": f"scrapper:data-NYSE-F/standard/{SLUG}", "slug": SLUG,
                  "symbol": "NYSE:F", "timeframe": "15", "strategySha256": "c" * 64},
        "window": {"fromMs": 1743465600000, "toMs": 1777593600000},
        "symbols": {
            "SYN:DXY": {"canonical": "SYN:DXY", "valid": True,
                        "facts": {"type": "index", "timezone": "America/New_York",
                                  "session": "regular", "currency": "USD", "mintick": 0.001},
                        "factsSha256": FACTS_SHA},
            "NYSE:F": {"canonical": "NYSE:F", "valid": True,
                       "facts": {"type": "stock", "timezone": "America/New_York",
                                 "session": "regular", "currency": "USD", "mintick": 0.01},
                       "factsSha256": FACTS_SHA},
            "SYN:GONE": {"canonical": None, "valid": False, "facts": None,
                         "factsSha256": FACTS_SHA},
        },
        "feeds": [{"symbol": "SYN:DXY", "timeframe": "15", "sha256": _sha(FEED_CSV),
                   "bytes": len(FEED_CSV),
                   "columns": ["timestamp", "time_close", "open", "high", "low", "close",
                               "volume", "fp_delta_100_70"],
                   "provenanceSha256": PROV_SHA}],
        "recorded": [{"key": "earnings|NYSE:F|actual|-|gaps_on|lookahead_off",
                      "sha256": _sha(TAPE_CSV), "columns": ["chart_open_ms", "value"],
                      "provenanceSha256": PROV_SHA}],
        "libraries": [{"import": "xsymd/Lib/1", "id": "PUB;" + "d" * 32, "version": "1.0",
                       "access": "open_no_auth", "sha256": _sha(LIBRARY), "licenseLine": None,
                       "requires": [], "provenanceSha256": PROV_SHA}],
    }
    doc.update(overrides)
    return doc


def _materialize(root: Path, doc: dict, slug: str = SLUG,
                 files: tuple[bytes, ...] = (FEED_CSV, TAPE_CSV, LIBRARY)) -> Path:
    probe = root / slug
    (probe / "files").mkdir(parents=True, exist_ok=True)
    (probe / "requests.json").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    for data in files:
        (probe / "files" / _sha(data)).write_bytes(data)
    return probe


class _FakeLibrary:
    """The four setters and the run, recording the call order."""

    def __init__(self) -> None:
        self.events = []

    def strategy_create(self, _params):
        self.events.append(("create",))
        return 7

    def run_backtest_full(self, _state, _bars, count, *_rest):
        self.events.append(("run", count))

    def strategy_get_last_error(self, _state):
        return b""

    def report_free(self, _report):
        pass

    def strategy_free(self, _state):
        pass

    def strategy_set_symbol_facts(self, _state, key, field, value):
        self.events.append(("facts", key.decode(), field.decode(), value.decode()))
        return 0

    def strategy_set_symbol_feed(self, _state, key, timeframe, bars, close_ms, count):
        self.events.append(("feed", key.decode(), timeframe.decode(), count,
                            [bars[i].timestamp for i in range(count)],
                            [close_ms[i] for i in range(count)],
                            [bars[i].volume for i in range(count)]))
        return 0

    def strategy_set_symbol_feed_column(self, _state, key, timeframe, name, values, count):
        self.events.append(("column", key.decode(), timeframe.decode(), name.decode(),
                            [values[i] for i in range(count)]))
        return 0

    def strategy_set_recorded_series(self, _state, key, open_ms, values, count):
        self.events.append(("recorded", key.decode(), [open_ms[i] for i in range(count)],
                            [values[i] for i in range(count)]))
        return 0


class _NoSettersLibrary(_FakeLibrary):
    def __getattribute__(self, name):
        if name in rs._REQUESTS_EXPORTS:
            raise AttributeError(name)
        return super().__getattribute__(name)


def _strategy(fake) -> Strategy:
    strategy = Strategy.__new__(Strategy)
    strategy.lib = fake
    return strategy


def _chart(root: Path) -> Path:
    chart = root / "chart.csv"
    chart.write_text("timestamp,open,high,low,close,volume\n"
                     "1744032600000,9.06,9.1,9.0,9.05,100\n"
                     "1744033500000,9.05,9.2,9.0,9.18,120\n", encoding="utf-8")
    return chart


class LoadRequests(unittest.TestCase):
    def test_unset_or_absent_reads_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertIsNone(load_probe_requests(root / SLUG, environ={}))
            self.assertIsNone(load_probe_requests(root / SLUG,
                                                  environ={REQUESTS_ROOT_ENV: ""}))
            # Set, but the case pinned nothing for this probe.
            _materialize(root, _manifest(), slug="another-probe")
            self.assertIsNone(load_probe_requests(Path("/build") / SLUG,
                                                  environ={REQUESTS_ROOT_ENV: str(root)}))

    def test_manifest_is_verified_and_parsed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _materialize(root, _manifest())
            # The probe is the basename of the strategy directory it is handed
            # (the verifier's BUILD/<slug>), wherever that directory lives.
            requests = load_probe_requests(Path("/elsewhere/build") / SLUG,
                                           environ={REQUESTS_ROOT_ENV: str(root)})
            self.assertIsNotNone(requests)
            self.assertEqual(requests["slug"], SLUG)
            feed = requests["feeds"][0]
            self.assertEqual((feed["symbol"], feed["timeframe"], feed["bars"]),
                             ("SYN:DXY", "15", 3))
            self.assertEqual(feed["columns"], ["fp_delta_100_70"])
            self.assertEqual([feed["bar_array"][i].timestamp for i in range(3)],
                             [1743984000000, 1743984900000, 1743985800000])
            self.assertEqual([feed["close_ms"][i] for i in range(3)],
                             [1743984900000, 1743985800000, 1743986700000])
            # An empty or NaN volume and an empty extra value are NaN.
            self.assertTrue(math.isnan(feed["bar_array"][0].volume))
            self.assertTrue(math.isnan(feed["bar_array"][1].volume))
            self.assertEqual(feed["bar_array"][2].volume, 5.0)
            name, values = feed["extras"][0]
            self.assertEqual(name, "fp_delta_100_70")
            self.assertEqual(values[0], 12.0)
            self.assertTrue(math.isnan(values[1]))
            self.assertEqual(values[2], -3.5)
            tape = requests["recorded"][0]
            self.assertEqual(tape["rows"], 2)
            self.assertEqual([tape["values"][i] for i in range(2)], [1.65, 1.57])
            facts = {s["symbol"]: dict(s["facts"]) for s in requests["symbols"]}
            self.assertEqual(facts["SYN:DXY"]["type"], "index")
            self.assertEqual(facts["SYN:DXY"]["mintick"], "0.001")
            self.assertEqual(facts["SYN:DXY"]["valid"], "true")
            self.assertEqual(facts["SYN:GONE"], {"valid": "false"})
            provenance = probe_requests_provenance(requests)
            self.assertEqual(provenance["feeds"][0]["sha256"], _sha(FEED_CSV))
            self.assertEqual(provenance["feeds"][0]["bars"], 3)
            self.assertEqual(provenance["recorded"][0]["rows"], 2)
            self.assertEqual(provenance["libraries"][0]["sha256"], _sha(LIBRARY))
            json.dumps(provenance)  # JSON-clean for the fingerprint

    def _refused(self, root: Path, pattern: str) -> None:
        with self.assertRaisesRegex(RequestsManifestError, pattern):
            load_probe_requests(Path("/build") / SLUG, environ={REQUESTS_ROOT_ENV: str(root)})

    def test_malformed_manifests_are_refused_by_name(self) -> None:
        cases = [
            (_manifest(schemaVersion="pineforge-probe-requests/v2"), "unsupported schema"),
            (dict(_manifest(), extra=1), "must carry exactly"),
            (_manifest(window={"fromMs": 5, "toMs": 5}), "window must be"),
            (_manifest(probe=dict(_manifest()["probe"], slug="someone-else")),
             "is not this probe's"),
            (_manifest(feeds=[dict(_manifest()["feeds"][0], timeframe="D")]),
             r"timeframe 'D' is not canonical"),
            (_manifest(feeds=[dict(_manifest()["feeds"][0], symbol="SYN:GONE")]),
             "is not a valid entry of symbols"),
            (_manifest(feeds=[dict(_manifest()["feeds"][0], bytes=len(FEED_CSV) + 1)]),
             "bytes, the manifest says"),
            (_manifest(recorded=[dict(_manifest()["recorded"][0],
                                      key="earnings|NYSE:F|actual|FQ|gaps_on|lookahead_off")]),
             "takes no period"),
            (_manifest(libraries=[dict(_manifest()["libraries"][0], requires=["xsymd/Dep/1"])]),
             "which the manifest does not pin"),
            (_manifest(symbols=dict(_manifest()["symbols"], **{
                "SYN:BAD": {"canonical": "SYN:BAD", "valid": False, "facts": None,
                            "factsSha256": FACTS_SHA}})),
             "canonical and facts must be null"),
        ]
        for doc, pattern in cases:
            with self.subTest(pattern=pattern), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                _materialize(root, doc)
                self._refused(root, pattern)

    def test_files_are_verified(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe = _materialize(root, _manifest())
            (probe / "files" / _sha(TAPE_CSV)).write_bytes(TAPE_CSV + b"1760000000000,0\n")
            self._refused(root, "does not hash to its name")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _materialize(root, _manifest(), files=(FEED_CSV, TAPE_CSV))
            self._refused(root, r"library xsymd/Lib/1 names files/[0-9a-f]{64}, which is missing")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad = FEED_CSV.replace(b"fp_delta_100_70", b"fp_delta_100_71")
            doc = _manifest(feeds=[dict(_manifest()["feeds"][0], sha256=_sha(bad),
                                        bytes=len(bad))])
            _materialize(root, doc, files=(bad, TAPE_CSV, LIBRARY))
            self._refused(root, "header is not")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / SLUG).mkdir()
            (root / SLUG / "requests.json").write_bytes(b"{not json")
            self._refused(root, "not valid UTF-8 JSON")

    def test_unwalkable_manifests_are_refused_by_name(self) -> None:
        # A mintick past a double, and JSON nested past the recursion limit,
        # raise from the walk itself (OverflowError, RecursionError).
        huge = _manifest()
        huge["symbols"]["SYN:DXY"]["facts"]["mintick"] = 10 ** 400
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _materialize(root, huge)
            self._refused(root, r"facts\.mintick must be a positive finite number")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / SLUG).mkdir()
            (root / SLUG / "requests.json").write_bytes(b"[" * 100000 + b"]" * 100000)
            self._refused(root, r"requests manifest cannot be read \(RecursionError")
        # A stamp an int64 cannot hold, which ctypes would wrap silently.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad = FEED_CSV.replace(b"1743984000000,1743984900000",
                                   b"99999999999999999999,1743984900000")
            doc = _manifest(feeds=[dict(_manifest()["feeds"][0], sha256=_sha(bad),
                                        bytes=len(bad))])
            _materialize(root, doc, files=(bad, TAPE_CSV, LIBRARY))
            self._refused(root, "row 1 does not parse")

    def test_a_header_only_tape_installs_as_a_series_without_rows(self) -> None:
        # The workflow keeps a request that was na on every chart bar as a tape
        # with a header only; it is installed (n == 0), never skipped, so the
        # engine reads na on every bar instead of failing the key as unknown.
        empty = b"chart_open_ms,value\n"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            doc = _manifest(recorded=[dict(_manifest()["recorded"][0], sha256=_sha(empty))])
            _materialize(root, doc, files=(FEED_CSV, empty, LIBRARY))
            requests = load_probe_requests(Path("/build") / SLUG,
                                           environ={REQUESTS_ROOT_ENV: str(root)})
            self.assertEqual(requests["recorded"][0]["rows"], 0)
            fake = _FakeLibrary()
            _strategy(fake)._install_probe_requests(7, requests)
            self.assertIn(("recorded", "earnings|NYSE:F|actual|-|gaps_on|lookahead_off", [], []),
                          fake.events)

    def test_only_the_probes_own_directory_is_read(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _materialize(root, _manifest())
            # A sibling probe's broken manifest, and a case-wide library
            # directory that points nowhere useful: neither is opened.
            (root / "sibling").mkdir()
            (root / "sibling" / "requests.json").write_text("{broken", encoding="utf-8")
            opened = []
            real_open = Path.read_bytes

            def spy(path):
                opened.append(str(path))
                return real_open(path)

            env = {REQUESTS_ROOT_ENV: str(root), "PINEFORGE_PINE_LIBRARIES": str(root / "nowhere")}
            with mock.patch.object(Path, "read_bytes", spy):
                self.assertIsNotNone(load_probe_requests(Path("/build") / SLUG, environ=env))
            self.assertTrue(opened)
            for path in opened:
                self.assertTrue(path.startswith(str(root / SLUG) + "/"), path)

    def test_validator_returns_every_referenced_file(self) -> None:
        files = validate_probe_requests(_manifest())
        self.assertEqual([f["role"] for f in files], ["feed", "recorded", "library"])


class InstallRequests(unittest.TestCase):
    def test_installs_before_the_run_and_reports_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _materialize(root, _manifest())
            requests = load_probe_requests(Path("/b") / SLUG, environ={REQUESTS_ROOT_ENV: str(root)})
            fake = _FakeLibrary()
            result = _strategy(fake).run(_chart(root), probe_requests=requests)
            kinds = [event[0] for event in fake.events]
            self.assertEqual(kinds[0], "create")
            self.assertEqual(kinds[-1], "run")
            self.assertLess(kinds.index("recorded"), kinds.index("run"))
            self.assertLess(kinds.index("feed"), kinds.index("column"))
            facts = [e[1:] for e in fake.events if e[0] == "facts"]
            self.assertIn(("SYN:DXY", "type", "index"), facts)
            self.assertIn(("SYN:DXY", "mintick", "0.001"), facts)
            self.assertIn(("SYN:GONE", "valid", "false"), facts)
            feed = next(e for e in fake.events if e[0] == "feed")
            self.assertEqual(feed[1:4], ("SYN:DXY", "15", 3))
            self.assertEqual(feed[5], [1743984900000, 1743985800000, 1743986700000])
            column = next(e for e in fake.events if e[0] == "column")
            self.assertEqual(column[1:4], ("SYN:DXY", "15", "fp_delta_100_70"))
            recorded = next(e for e in fake.events if e[0] == "recorded")
            self.assertEqual(recorded[1], "earnings|NYSE:F|actual|-|gaps_on|lookahead_off")
            self.assertEqual(recorded[2], [1746192600000, 1754055000000])
            self.assertEqual(result["probe_requests"]["feeds"][0]["sha256"], _sha(FEED_CSV))

    def test_a_library_without_the_setters_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _materialize(root, _manifest())
            requests = load_probe_requests(Path("/b") / SLUG, environ={REQUESTS_ROOT_ENV: str(root)})
            fake = _NoSettersLibrary()
            with self.assertRaisesRegex(RuntimeError, "lacks the symbol-data request setters"):
                _strategy(fake).run(_chart(root), probe_requests=requests)
            self.assertEqual(fake.events, [])

    def test_runtime_provenance_is_unchanged_without_requests(self) -> None:
        runtime = build_runtime_provenance({}, None)
        self.assertNotIn("requests", runtime)


class _FakeStrategy:
    calls: list[dict] = []

    def __init__(self, so_path: Path) -> None:
        self.lib = None

    def run(self, bars_csv: Path, params=None, **kwargs) -> dict:
        _FakeStrategy.calls.append(dict(kwargs))
        return {"trades": [], "trace": [], "trace_names": [], "net_profit": 0.0,
                "input_bars_processed": 0}


def _run_main(strategy_dir: Path, feed: Path, env: dict, *extra: str) -> tuple[int, str]:
    _FakeStrategy.calls.clear()
    argv = ["run_strategy.py", str(strategy_dir), "--ohlcv", str(feed),
            "-o", str(strategy_dir / "out.csv"), *extra]
    buf = io.StringIO()
    with mock.patch.object(rs, "ensure_derived", lambda: None), \
            mock.patch.object(rs, "find_strategy_lib",
                              lambda d, so_name="strategy.so": d / so_name), \
            mock.patch.object(rs, "Strategy", _FakeStrategy), \
            mock.patch.object(rs, "REFERENCE_OHLCV", feed), \
            mock.patch.object(sys, "argv", argv), \
            mock.patch.dict(rs.os.environ, env, clear=False), \
            contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        try:
            rc = rs.main()
        except SystemExit as exit_:
            rc = exit_.code
    return rc, buf.getvalue()


class MainContract(unittest.TestCase):
    def test_unset_hands_the_engine_what_it_always_did(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            strategy_dir = root / SLUG
            strategy_dir.mkdir()
            with mock.patch.dict(rs.os.environ, {}, clear=False):
                rs.os.environ.pop(REQUESTS_ROOT_ENV, None)
                rc, _ = _run_main(strategy_dir, _chart(root), {})
            self.assertEqual(rc, 0)
            self.assertNotIn("probe_requests", _FakeStrategy.calls[0])

    def test_set_passes_the_verified_requests(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            requests_root = root / "requests"
            _materialize(requests_root, _manifest())
            strategy_dir = root / "build" / SLUG
            strategy_dir.mkdir(parents=True)
            rc, _ = _run_main(strategy_dir, _chart(root), {REQUESTS_ROOT_ENV: str(requests_root)})
            self.assertEqual(rc, 0)
            handed = _FakeStrategy.calls[0]["probe_requests"]
            self.assertEqual(handed["slug"], SLUG)

    def test_a_malformed_manifest_stops_the_run_by_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            requests_root = root / "requests"
            _materialize(requests_root, _manifest(schemaVersion="nope"))
            strategy_dir = root / "build" / SLUG
            strategy_dir.mkdir(parents=True)
            rc, output = _run_main(strategy_dir, _chart(root),
                                   {REQUESTS_ROOT_ENV: str(requests_root)})
            self.assertNotEqual(rc, 0)
            self.assertIn("refused requests manifest", str(rc) + output)
            self.assertIn("unsupported schema", str(rc) + output)
            self.assertEqual(_FakeStrategy.calls, [])

    def test_the_docker_runner_refuses_pinned_request_data(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            requests_root = root / "requests"
            _materialize(requests_root, _manifest())
            strategy_dir = root / "build" / SLUG
            strategy_dir.mkdir(parents=True)
            rc, output = _run_main(strategy_dir, _chart(root),
                                   {REQUESTS_ROOT_ENV: str(requests_root)}, "--runner", "docker")
            self.assertIn("does not support pinned request data", str(rc) + output)


if __name__ == "__main__":
    unittest.main()
