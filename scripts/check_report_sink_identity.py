#!/usr/bin/env python3
"""CR-150 lane L0, acceptance (d), corpus half (DESIGN.md section 7).

Every corpus probe, sink off vs sink on:
  * raw pf_metrics_t values (the JSON doubles are round-trip exact, so the
    comparison is bit for bit with struct.pack("<d", value)),
  * every E (equity point) and T (trade) value,
  * run_json's full JSON (sink off) against the summary plus the sink
    (sink on), masked as in DESIGN.md 4.2,
  * CSV input against .bin input when run_json has csv2bin.

RED ON 3fc010a8b: run_json has no --report-sink and the runtime export
strategy_set_report_sink_v1 is missing, so this script exits 1 before reading
the corpus. That is the feature under test not existing, never a setup error.

Exit codes: 0 identical, 1 a difference (or the missing feature), 2 the check
could not run (corpus absent, module or feed missing, run_json failed).

Also, --self-check: the parts of acceptance (i) that need the installed
run_json path. It (a) checks the F9 NaN rule for the bar file: a -nan volume
cell keeps the bytes struct.pack("<5dq", float("-nan"), ...) produced, because
that is the canonical values hash payload; and (b) with --so/--ohlcv, writes a
small sink through the real module, cuts it at every line boundary, and holds
a reference reader (this file's SinkReader) to refusing each cut, and checks
the trailer sha256 against hashlib.

Usage:
  python3 scripts/check_report_sink_identity.py --corpus corpus --build-dir build
  python3 scripts/check_report_sink_identity.py --so DIR/strategy.so \\
      --ohlcv DIR/ohlcv.csv --label one-probe
  python3 scripts/check_report_sink_identity.py --self-check --so ... --ohlcv ...
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
META_KEYS = frozenset({
    "tv_trades_csv", "tv_trades_csv_tz", "runtime_overrides", "strategy_overrides",
    "validation_overrides", "ohlcv_csv", "aux_security_ohlcv_csv",
    "aux_security_input_tf", "input_tf", "script_tf", "ohlcv_start_ms",
    "inputs", "overrides",
})
MASKED_KEYS = frozenset({
    "elapsed_seconds", "timing", "throughput", "phase_timing", "fingerprint",
    "source_bytes_sha256", "evaluated_source_values_sha256", "ohlcv",
    "report_sink",
})


class CheckFailure(AssertionError):
    pass


class SetupFailure(RuntimeError):
    pass


def mask(value):
    if isinstance(value, dict):
        return {k: mask(v) for k, v in value.items() if k not in MASKED_KEYS}
    if isinstance(value, list):
        return [mask(v) for v in value]
    return value


def same_float(left, right) -> bool:
    if left is None or right is None:
        return left is right
    return struct.pack("<d", float(left)) == struct.pack("<d", float(right))


def compare_values(where: str, left, right) -> None:
    if isinstance(left, dict) and isinstance(right, dict):
        if set(left) != set(right):
            raise CheckFailure(f"{where}: keys differ: {sorted(set(left) ^ set(right))}")
        for key in left:
            compare_values(f"{where}.{key}", left[key], right[key])
        return
    if isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            raise CheckFailure(f"{where}: lengths differ {len(left)} != {len(right)}")
        for index, (a, b) in enumerate(zip(left, right)):
            compare_values(f"{where}[{index}]", a, b)
        return
    if isinstance(left, float) or isinstance(right, float):
        if not same_float(left, right):
            raise CheckFailure(f"{where}: {left!r} != {right!r}")
        return
    if left != right:
        raise CheckFailure(f"{where}: {left!r} != {right!r}")


def run_json_help(run_json: Path) -> str:
    result = subprocess.run([sys.executable, str(run_json), "--help"],
                            capture_output=True, text=True, check=False)
    return (result.stdout or "") + (result.stderr or "")


def run_json(run_json: Path, args: list[str], where: str) -> str:
    result = subprocess.run([sys.executable, str(run_json), *args],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SetupFailure(
            f"{where}: run_json exited {result.returncode}: "
            f"{(result.stderr or result.stdout)[-4000:]}")
    return result.stdout


# ── the reference sink reader (DESIGN.md 3.7, 4.4) ──────────────────────


class SinkReader:
    """Open, check the header, iterate typed records, verify the trailer.

    The trailer is verified only after every line was read: a truncated sink
    raises TruncatedSink on the last line or on the missing trailer.
    """

    class Error(RuntimeError):
        pass

    def __init__(self, path: Path):
        self.path = path
        self._handle = path.open("r", encoding="utf-8")
        self.header = json.loads(self._handle.readline())
        if self.header.get("schema") != "pf-report-sink" or self.header.get("version") != 1:
            raise SinkReader.Error("missing or unknown header")
        self.types = {
            record["type"]: record
            for record in self.header.get("records", [])
        }
        if "E" not in self.types or "T" not in self.types:
            raise SinkReader.Error("the header does not declare E and T")
        self.digest = hashlib.sha256()
        self.digest.update(self._line_bytes(0))
        self.counts = {tag: 0 for tag in self.types}
        self._expected_elements = {
            tag: 1 + len(record["fields"]) for tag, record in self.types.items()
        }
        self._done = False

    def _line_bytes(self, index: int) -> bytes:
        # Re-reading the header is cheap and keeps the digest exact.
        self._handle.seek(0)
        for _ in range(index):
            self._handle.readline()
        line = self._handle.readline()
        return line.encode("utf-8")

    def __iter__(self):
        try:
            for line in self._handle:
                if not line.endswith("\n"):
                    raise SinkReader.Error("the last line has no newline (truncated)")
                if line.startswith("{\"trailer\""):
                    self._verify_trailer(line)
                    self._done = True
                    return
                record = json.loads(line)
                if not isinstance(record, list) or not record:
                    raise SinkReader.Error("a record line is not a non-empty array")
                tag = record[0]
                if tag not in self.types:
                    raise SinkReader.Error(f"record tag {tag!r} is not in the header")
                if len(record) != self._expected_elements[tag]:
                    raise SinkReader.Error(
                        f"{tag} record has {len(record)} elements, the header "
                        f"declares {self._expected_elements[tag]}")
                self.counts[tag] += 1
                self.digest.update(line.encode("utf-8"))
                yield record
            raise SinkReader.Error("the sink has no trailer (truncated)")
        finally:
            self._handle.close()

    def verify(self) -> dict:
        if not self._done:
            raise SinkReader.Error("the trailer was not verified")
        return self.counts

    def _verify_trailer(self, line: str) -> None:
        trailer = json.loads(line)
        if trailer.get("trailer") != "pf-report-sink" or trailer.get("version") != 1:
            raise SinkReader.Error("the last line is not a v1 trailer")
        if trailer.get("counts") != self.counts:
            raise SinkReader.Error(
                f"trailer counts {trailer.get('counts')} != read {self.counts}")
        if trailer.get("sha256") != self.digest.hexdigest():
            raise SinkReader.Error("trailer sha256 does not match the lines before it")


def read_sink(path: Path) -> tuple[dict, dict[str, list[list]]]:
    reader = SinkReader(path)
    records: dict[str, list[list]] = {}
    for record in reader:
        records.setdefault(record[0], []).append(record)
    reader.verify()
    return reader.header, records


# ── probe discovery and invocation ──────────────────────────────────────


def load_params(probe: Path) -> dict:
    path = probe / "inputs.json"
    if not path.is_file():
        return {}
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return value if isinstance(value, dict) else {}


def probe_feed(probe: Path, params: dict, corpus: Path) -> Path:
    if params.get("ohlcv_csv"):
        candidate = Path(str(params["ohlcv_csv"]))
        if not candidate.is_absolute():
            candidate = probe / candidate
        if candidate.is_file():
            return candidate
    for name in ("ohlcv.csv", "data.csv"):
        candidate = probe / name
        if candidate.is_file():
            return candidate
    fallback = corpus / "data" / "derived" / "ohlcv_ETH-USDT-USDT_15m_window.csv"
    if fallback.is_file():
        return fallback
    raise SetupFailure(f"{probe}: no OHLCV feed found")


def probe_inputs(params: dict) -> dict:
    inputs = {}
    for key, value in params.items():
        if key in META_KEYS or isinstance(value, (bool, type(None))):
            continue
        if isinstance(value, (str, int, float)):
            inputs[key] = value
    nested = params.get("inputs")
    if isinstance(nested, dict):
        for key, value in nested.items():
            if isinstance(value, (str, int, float)) and not isinstance(value, bool):
                inputs[key] = value
    return inputs


def build_module_index(build_dir: Path) -> dict[str, Path]:
    index: dict[str, Path] = {}
    if not build_dir.is_dir():
        return index
    for path in build_dir.rglob("strategy.so"):
        parts = path.parts
        for name in reversed(parts[:-1]):
            index.setdefault(name, path)
            if path.parent.name == name:
                break
    for path in build_dir.rglob("*.so"):
        index.setdefault(path.stem, path)
    return index


def probe_module(probe: Path, build_dir: Path, index: dict[str, Path]) -> Path:
    direct = probe / "strategy.so"
    if direct.is_file():
        return direct
    for candidate in index.values():
        if probe.name in candidate.parts:
            return candidate
    named = index.get(probe.name)
    if named is not None:
        return named
    raise SetupFailure(
        f"{probe}: no strategy.so; the corpus build (--build-dir) did not "
        "produce it")


def base_args(so: Path, feed: Path, params: dict) -> list[str]:
    input_tf = str(params.get("input_tf") or "15")
    script_tf = str(params.get("script_tf") or input_tf)
    args = ["--so", str(so), "--ohlcv", str(feed),
            "--input-tf", input_tf, "--script-tf", script_tf]
    inputs = probe_inputs(params)
    if inputs:
        args += ["--inputs", json.dumps(inputs, sort_keys=True)]
    if params.get("ohlcv_start_ms") is not None:
        args += ["--trade-start-ms", str(int(params["ohlcv_start_ms"]))]
    return args


# ── the identity check ──────────────────────────────────────────────────


def check_one(run_json: Path, label: str, so: Path, feed: Path, params: dict,
              work: Path, use_bin: bool) -> None:
    args = base_args(so, feed, params)
    full_text = run_json(run_json, args, f"{label}/sink-off")
    full = json.loads(full_text)

    sink_path = work / f"{label}.sink"
    summary_text = run_json(run_json, args + [
        "--report-sink", str(sink_path), "--run-id", label,
        "--spill-dir", str(work)], f"{label}/sink-on")
    summary = json.loads(summary_text)

    header, records = read_sink(sink_path)
    # The summary carries the same metrics doubles, bit for bit.
    compare_values(f"{label}: metrics", full.get("metrics"), summary.get("metrics"))
    compare_values(f"{label}: summary", mask(full.get("summary")), mask(summary.get("summary")))

    curve = full.get("equity_curve") or []
    e_records = records.get("E", [])
    if len(e_records) != len(curve):
        raise CheckFailure(
            f"{label}: sink E count {len(e_records)} != full report curve {len(curve)}")
    for index, (record, point) in enumerate(zip(e_records, curve)):
        compare_values(f"{label}: E[{index}].time_ms", record[1], point["time_ms"])
        compare_values(f"{label}: E[{index}].equity", record[2], point["equity"])
        compare_values(f"{label}: E[{index}].open_profit", record[3],
                       point["open_profit"])

    trades = full.get("trades") or []
    t_records = records.get("T", [])
    if len(t_records) != len(trades):
        raise CheckFailure(
            f"{label}: sink T count {len(t_records)} != full report trades "
            f"{len(trades)}")
    # The first sixteen T fields are today's run_json trade dict, same names
    # and order (DESIGN.md 3.7).
    t_keys = ("n", "side", "entry_time", "exit_time", "entry_price", "exit_price",
              "qty", "pnl", "pnl_pct", "max_runup", "max_drawdown", "commission",
              "entry_bar_index", "exit_bar_index", "open_at_end")
    for index, (record, trade) in enumerate(zip(t_records, trades)):
        for offset, key in enumerate(t_keys):
            compare_values(f"{label}: T[{index}].{key}", record[offset + 1],
                           trade[key])

    # CSV input against .bin input (DESIGN.md 1, 7 (d)).
    if use_bin:
        bin_path = work / f"{label}.bin"
        convert = subprocess.run(
            [sys.executable, str(run_json), "csv2bin", "--chart",
             "--symbol", params.get("symbol", "TEST:PROBE"),
             "--tf", str(params.get("input_tf") or "15"), str(feed), str(bin_path)],
            capture_output=True, text=True, check=False)
        if convert.returncode != 0:
            raise SetupFailure(
                f"{label}: csv2bin failed: {(convert.stderr or convert.stdout)[-2000:]}")
        bin_args = ["--so", str(so), "--ohlcv-bin", str(bin_path),
                    "--input-tf", str(params.get("input_tf") or "15"),
                    "--script-tf", str(params.get("script_tf") or
                                       params.get("input_tf") or "15")]
        inputs = probe_inputs(params)
        if inputs:
            bin_args += ["--inputs", json.dumps(inputs, sort_keys=True)]
        if params.get("ohlcv_start_ms") is not None:
            bin_args += ["--trade-start-ms", str(int(params["ohlcv_start_ms"]))]
        bin_doc = json.loads(run_json(run_json, bin_args, f"{label}/bin"))
        compare_values(f"{label}: CSV vs .bin", mask(full), mask(bin_doc))
    print(f"  {label}: identical (E={len(e_records)}, T={len(t_records)})")


# ── self-check: the Python half of acceptance (i) ───────────────────────


def self_check(run_json: Path, so: Path | None, feed: Path | None, work: Path) -> None:
    # (a) The -nan volume cell keeps the bytes float('-nan') hashes today.
    nan_path = work / "nan.bin"
    value = float("-nan")
    payload = struct.pack("<5dq", 100.0, 101.0, 99.0, 100.5, value, 1700000000000)
    nan_path.write_bytes(payload)
    reread = struct.unpack("<5dq", nan_path.read_bytes())
    if struct.pack("<5dq", *reread) != payload:
        raise CheckFailure("-nan volume does not round-trip the parsed bytes")
    if struct.pack("<d", value) != b"\x00\x00\x00\x00\x00\x00\xf8\xff":
        # 0xFFF8000000000000: negative quiet NaN, the byte pattern
        # struct.pack gives float('-nan') (DESIGN.md 1.4).
        raise CheckFailure(f"float('-nan') packed as {struct.pack('<d', value)!r}")
    print("  -nan volume: bytes round-trip")

    if so is None or feed is None:
        print("  (real-module sink checks need --so and --ohlcv; skipped)")
        return

    sink_path = work / "format.sink"
    document = json.loads(run_json(
        run_json,
        ["--so", str(so), "--ohlcv", str(feed), "--input-tf", "1",
         "--script-tf", "1", "--report-sink", str(sink_path),
         "--run-id", "l0-self-check", "--spill-dir", str(work)],
        "self-check/sink"))
    header, records = read_sink(sink_path)
    if header.get("run_id") != "l0-self-check":
        raise CheckFailure("the header run_id does not match --run-id")
    if header.get("engine") in (None, ""):
        raise CheckFailure("the header has no engine version")
    print(f"  header: {len(header['records'])} record types; "
          f"E={len(records.get('E', []))}, T={len(records.get('T', []))}")

    # The trailer's sha256 equals hashlib over every byte before it.
    raw = sink_path.read_bytes()
    lines = raw.split(b"\n")
    if lines[-1] == b"":
        lines.pop()
    if not lines[-1].startswith(b"{\"trailer\""):
        raise CheckFailure("the last line is not a trailer")
    prefix = b"".join(line + b"\n" for line in lines[:-1])
    trailer = json.loads(lines[-1])
    if trailer.get("sha256") != hashlib.sha256(prefix).hexdigest():
        raise CheckFailure("the trailer sha256 does not equal hashlib over the prefix")

    # The reference reader refuses a file cut at every line boundary.
    boundaries = [0]
    offset = 0
    for line in lines:
        offset += len(line) + 1
        boundaries.append(offset)
    for cut in boundaries[:-1]:
        truncated = work / "truncated.sink"
        truncated.write_bytes(raw[:cut])
        try:
            read_sink(truncated)
        except (SinkReader.Error, ValueError, json.JSONDecodeError):
            continue
        raise CheckFailure(f"the reader accepted a sink cut at byte {cut}")
    print(f"  trailer: sha256 matches hashlib; {len(boundaries) - 1} cuts refused")


# ── main ────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-json", type=Path, default=ROOT / "docker" / "run_json.py")
    parser.add_argument("--corpus", type=Path, default=ROOT / "corpus")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--probes", default="", help="comma-separated probe names")
    parser.add_argument("--so", type=Path, default=None, help="one explicit module")
    parser.add_argument("--ohlcv", type=Path, default=None, help="one explicit feed")
    parser.add_argument("--label", default="explicit")
    parser.add_argument("--self-check", action="store_true")
    parser.add_argument("--work-dir", type=Path, default=None)
    args = parser.parse_args(argv)

    help_text = run_json_help(args.run_json)
    if "--report-sink" not in help_text:
        print("check_report_sink_identity: strategy_set_report_sink_v1 is missing "
              "(docker/run_json.py has no --report-sink). The 1.7.0 report sink "
              "does not exist yet: acceptance (d) is red.", file=sys.stderr)
        return 1
    use_bin = "--ohlcv-bin" in help_text and "csv2bin" in help_text

    work = args.work_dir or Path(tempfile.mkdtemp(prefix="pf-sink-identity-"))
    work.mkdir(parents=True, exist_ok=True)

    if args.self_check:
        try:
            self_check(args.run_json, args.so, args.ohlcv, work)
        except (CheckFailure, SetupFailure, SinkReader.Error) as error:
            print(f"self-check FAILED: {error}", file=sys.stderr)
            return 1 if isinstance(error, CheckFailure) else 2
        return 0

    probes: list[tuple[str, Path, Path, dict]] = []
    if args.so is not None or args.ohlcv is not None:
        if args.so is None or args.ohlcv is None:
            print("SETUP: --so and --ohlcv go together", file=sys.stderr)
            return 2
        probes.append((args.label, args.so, args.ohlcv, {}))
    else:
        validation = args.corpus / "validation"
        if not validation.is_dir():
            print(f"SETUP: {validation} is not present (the corpus submodule is "
                  "empty in this lane); run on the lab queue after checkout",
                  file=sys.stderr)
            return 2
        index = build_module_index(args.build_dir)
        wanted = {name.strip() for name in args.probes.split(",") if name.strip()}
        for probe in sorted(path for path in validation.iterdir() if path.is_dir()):
            if wanted and probe.name not in wanted:
                continue
            params = load_params(probe)
            try:
                module = probe_module(probe, args.build_dir, index)
                feed = probe_feed(probe, params, args.corpus)
            except SetupFailure as error:
                print(f"SETUP: {error}", file=sys.stderr)
                return 2
            probes.append((probe.name, module, feed, params))
        if not probes:
            print("SETUP: no corpus probes selected", file=sys.stderr)
            return 2

    failed = 0
    for label, module, feed, params in probes:
        try:
            check_one(args.run_json, label, module, feed, params, work, use_bin)
        except CheckFailure as error:
            failed += 1
            print(f"FAIL {label}: {error}", file=sys.stderr)
        except (SetupFailure, SinkReader.Error) as error:
            print(f"SETUP {label}: {error}", file=sys.stderr)
            return 2
    print(f"report-sink identity: {len(probes) - failed}/{len(probes)} probes identical")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
