"""Real native modules through the unmodified JSON CLI, including process RSS."""
from __future__ import annotations

import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "docker" / "run_json.py"


def exec_jailed_runner():
    """Set up inherited fd 5, then exec the actual CLI with the same PID."""
    import resource
    import signal

    if sys.argv[1:2] != ["--exec-jailed"] or len(sys.argv) < 4:
        raise SystemExit("expected --exec-jailed ARENA_FD followed by CLI arguments")
    arena = int(sys.argv[2])
    os.dup2(arena, 5, inheritable=True)
    if arena != 5:
        os.close(arena)
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
    os.execv(sys.executable, [sys.executable, "-B", str(RUNNER), *sys.argv[3:]])


# The launcher imports neither pytest nor run_json before exec. Preferences,
# admission, execution, publication and cleanup all belong to the real CLI.
if __name__ == "__main__":
    exec_jailed_runner()


import contextlib
import ctypes
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import math
import selectors
import statistics
import struct
import subprocess

import pytest


FIRST_MS = 1514764800000  # 2018-01-01 UTC
LAST_MS = 1704067200000   # 2024-01-01 UTC
DAY_MS = 86400000
SPAN_MINUTES = (LAST_MS - FIRST_MS) // 60000
SMALL_COUNT = (LAST_MS - FIRST_MS) // DAY_MS + 1
RSS_COUNTS = (1_000_000, 3_000_000)
RSS_REPEATS = 3
ARENA_BYTES = 64 * 1024 * 1024
RECORD = struct.Struct("<5dq")
RECEIPT_PREFIX = b"metrics-only-native "
OMITTED_TOP = {"trades", "equity_curve", "elapsed_seconds"}
OMITTED_DIAGNOSTICS = {"scratch_arena_bytes", "scratch_arena_high_water_bytes"}


@dataclass
class Run:
    stdout: bytes
    document: dict
    receipts: list[dict]
    peak_bytes: int


def native_module(certified):
    name = ("PINEFORGE_METRICS_ONLY_CERTIFIED_SO" if certified
            else "PINEFORGE_METRICS_ONLY_UNCERTIFIED_SO")
    value = os.environ.get(name)
    assert value, f"required native module not supplied: {name}"
    module = Path(value).resolve()
    assert module.is_file(), module
    library = ctypes.CDLL(str(module))
    probe = library.strategy_metrics_only_capability_v1
    probe.argtypes, probe.restype = [], ctypes.c_int32
    assert probe() == (0 if certified else -1), module
    return module


def write_tape(path, count):
    """Stream a real binary tape and its authentic sidecar, using bounded RAM."""
    assert 2 <= count <= SPAN_MINUTES + 1
    digest, buffer = hashlib.sha256(), bytearray()
    with path.open("wb") as output:
        for index in range(count):
            # Both RSS sizes span the same calendar population. Minute
            # quantization keeps labels increasing even at three million.
            timestamp = FIRST_MS + (SPAN_MINUTES * index // (count - 1)) * 60000
            day = (timestamp - FIRST_MS) // DAY_MS
            opened = 100. + .25 * (day % 120 - 60) + 3. * ((day // 120) % 2)
            closed = opened + (.5 if day % 5 < 3 else -.5)
            buffer.extend(RECORD.pack(
                opened, max(opened, closed) + 1., min(opened, closed) - 1.,
                closed, 10., timestamp))
            if len(buffer) >= 1024 * 1024:
                output.write(buffer)
                digest.update(buffer)
                buffer.clear()
        if buffer:
            output.write(buffer)
            digest.update(buffer)
    metadata = {
        "format": "pf_bar_t/v1", "record_size": RECORD.size,
        "symbol": "TEST:METRICS", "tf": "1", "count": count,
        "first_ts": FIRST_MS, "last_ts": LAST_MS, "sha256": digest.hexdigest(),
    }
    Path(str(path) + ".json").write_text(
        json.dumps(metadata, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8")
    return path


def sealed_arena_fd():
    flags = 1 | 2  # MFD_CLOEXEC | MFD_ALLOW_SEALING
    if hasattr(os, "memfd_create"):
        arena = os.memfd_create("pineforge-metrics-arena", flags)
    else:
        # Some relocatable Python builds omit os.memfd_create.
        libc = ctypes.CDLL(None, use_errno=True)
        libc.memfd_create.argtypes = [ctypes.c_char_p, ctypes.c_uint]
        libc.memfd_create.restype = ctypes.c_int
        arena = libc.memfd_create(b"pineforge-metrics-arena", flags)
        if arena < 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), "memfd_create")
    try:
        os.ftruncate(arena, ARENA_BYTES)
        seals = 0x0004 | 0x0002 | 0x0001  # GROW | SHRINK | SEAL
        fcntl.fcntl(arena, 1033, seals)  # F_ADD_SEALS
        assert fcntl.fcntl(arena, 1034) == seals  # F_GET_SEALS
        moved = fcntl.fcntl(arena, fcntl.F_DUPFD_CLOEXEC, 16)
    finally:
        os.close(arena)
    return moved


def wait4_capture(command, *, environment, cwd, pass_fds=()):
    """Drain both pipes, then reap this exact child once, preserving its rusage.

    communicate()/poll()/wait() must not reap it before wait4. In particular,
    subtracting RUSAGE_CHILDREN high-water marks cannot measure each run.
    """
    process = subprocess.Popen(
        command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env=environment, cwd=cwd, pass_fds=pass_fds)
    captured = [bytearray(), bytearray()]
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ, 0)
            selector.register(process.stderr, selectors.EVENT_READ, 1)
            while selector.get_map():
                for key, _events in selector.select():
                    chunk = os.read(key.fd, 65536)
                    if chunk:
                        captured[key.data].extend(chunk)
                    else:
                        selector.unregister(key.fileobj)
        waited, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
        assert waited == process.pid
    except BaseException:
        if process.returncode is None:
            process.kill()
            process.wait()
        raise
    finally:
        process.stdout.close()
        process.stderr.close()
    # Linux reports KiB; Darwin reports bytes. The RSS acceptance row is
    # Linux-only, while the default-profile document row also runs on Darwin.
    peak_bytes = int(usage.ru_maxrss) * (1 if sys.platform == "darwin" else 1024)
    return process.returncode, bytes(captured[0]), bytes(captured[1]), peak_bytes


def run_cli(tmp_path, module, tape, *, option=None, jailed=False):
    arguments = [
        "--so", str(module), "--ohlcv-bin", str(tape),
        "--input-tf", "1", "--script-tf", "1",
    ]
    if option is not None:
        arguments.append(option)
    if jailed:
        arguments += ["--profile", "jailed"]
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1",
                       PINEFORGE_METRICS_ONLY_TEST_RECEIPT="1")
    for name in ("PF_SCRATCH_ARENA_FD", "PF_SCRATCH_ARENA_BYTES",
                 "PF_SCRATCH_WINDOW_BYTES", "PF_SCRATCH_ARENA_JAILED",
                 "PINEFORGE_COMMAND_SCRATCH_DIR", "PINEFORGE_RUN_JSON_PROFILE"):
        environment.pop(name, None)
    work, temporary = tmp_path / "work", tmp_path / "tmp"
    work.mkdir(exist_ok=True)
    temporary.mkdir(exist_ok=True)
    environment.update(TMPDIR=str(temporary), TMP=str(temporary), TEMP=str(temporary))
    with contextlib.ExitStack() as cleanup:
        command = [sys.executable, "-B", str(RUNNER), *arguments]
        pass_fds = ()
        if jailed:
            assert sys.platform.startswith("linux"), "sealed-arena row requires Linux"
            arena = sealed_arena_fd()
            cleanup.callback(os.close, arena)
            environment.update(
                PF_SCRATCH_ARENA_FD="5", PF_SCRATCH_ARENA_BYTES=str(ARENA_BYTES),
                PF_SCRATCH_WINDOW_BYTES=str(1024 * 1024))
            command = [sys.executable, "-B", str(Path(__file__).resolve()),
                       "--exec-jailed", str(arena), *arguments]
            pass_fds = (arena,)
            for directory in (work, temporary):
                directory.chmod(0o555)
                cleanup.callback(directory.chmod, 0o755)
        before = sorted(str(path) for directory in (work, temporary)
                        for path in directory.rglob("*"))
        status, stdout, stderr, peak = wait4_capture(
            command, environment=environment, cwd=work, pass_fds=pass_fds)
        assert sorted(str(path) for directory in (work, temporary)
                      for path in directory.rglob("*")) == before
    assert status == 0, (command, status, stdout[-4000:], stderr)
    assert stdout.endswith(b"\n") and len(stdout.splitlines()) == 1, stdout[-4000:]
    receipts = []
    for line in stderr.splitlines():
        assert line.startswith(RECEIPT_PREFIX), stderr
        receipt = json.loads(line[len(RECEIPT_PREFIX):])
        assert receipt["status"] == 0, receipt
        receipts.append(receipt)
    assert receipts, "the real native handle was not observed during destruction"
    document = json.loads(stdout)
    assert document["diagnostics"]["run_profile"] == ("jailed" if jailed else "unjailed")
    assert document["diagnostics"]["scratch_arena_jailed"] is jailed
    return Run(stdout, document, receipts, peak)


def assert_route(run, *, streaming, count):
    # A discarded first attempt plus an authoritative replay is two receipts,
    # even when both final JSON documents happen to have identical bytes.
    assert len(run.receipts) == 1, run.receipts
    receipt = run.receipts[0]
    assert receipt["replays"] == 0, receipt
    if streaming:
        expected = {
            "route": 1, "reason": 11, "mode": 1, "mask": 0x37,
            "plain": 1, "failed": 0, "witness": 1, "mismatch": 0,
            "planned": 1, "count": count, "first": FIRST_MS, "last": LAST_MS,
        }
        for key, value in expected.items():
            assert receipt[key] == value, (key, receipt)
        assert receipt["attempt"] != 0, receipt
        assert receipt["authoritative_attempt"] == receipt["attempt"], receipt
    else:
        assert receipt["route"] == 2, receipt
        assert (receipt["mode"], receipt["mask"]) == (0, 0x7f), receipt
        assert receipt["plain"] == receipt["planned"] == 0, receipt


def assert_nontrivial(document, count):
    assert document["input"]["bars"] == count, document["input"]
    summary, metrics = document["summary"], document["metrics"]
    assert summary["bars_processed"] == count, summary
    assert 2 <= summary["total_trades"] <= 16, summary
    assert summary["wins"] > 0 and summary["losses"] > 0, summary
    assert summary["net_pnl"] != 0, summary
    assert metrics["longs"]["num_trades"] > 0, metrics["longs"]
    assert metrics["shorts"]["num_trades"] > 0, metrics["shorts"]
    for name in ("sharpe_tv", "sortino_tv", "sharpe_bar", "sortino_bar", "cagr"):
        value = metrics["equity"][name]
        assert value is not None and math.isfinite(value), (name, metrics["equity"])


def check_documents(tmp_path, *, certified, jailed):
    module = native_module(certified)
    tape = write_tape(tmp_path / "daily.bin", SMALL_COUNT)
    auto = run_cli(tmp_path, module, tape, option="--metrics-only", jailed=jailed)
    legacy = run_cli(tmp_path, module, tape, option="--metrics-only=legacy", jailed=jailed)
    ordinary = run_cli(tmp_path, module, tape, jailed=jailed)

    # This comparator consumes untouched stdout bytes, including the newline.
    # Schema inspection below is independent and never normalizes this pair.
    assert auto.stdout == legacy.stdout
    assert_nontrivial(auto.document, SMALL_COUNT)
    full, scalar = ordinary.document, auto.document
    assert set(full) - set(scalar) == OMITTED_TOP
    assert not set(scalar) - set(full)
    assert set(full["diagnostics"]) - set(scalar["diagnostics"]) == OMITTED_DIAGNOSTICS
    assert not set(scalar["diagnostics"]) - set(full["diagnostics"])
    projection = {key: value for key, value in full.items() if key not in OMITTED_TOP}
    projection["diagnostics"] = {
        key: value for key, value in full["diagnostics"].items()
        if key not in OMITTED_DIAGNOSTICS}
    assert scalar == projection
    assert isinstance(full["trades"], list) and full["trades"]
    assert len(full["equity_curve"]) == SMALL_COUNT
    months = {
        datetime.fromtimestamp(point["time_ms"] / 1000, timezone.utc).strftime("%Y-%m")
        for point in full["equity_curve"]}
    assert len(months) >= 60, months
    assert type(full["elapsed_seconds"]) in (int, float)
    if jailed:
        assert full["diagnostics"]["scratch_arena_bytes"] == ARENA_BYTES
        assert 0 <= full["diagnostics"]["scratch_arena_high_water_bytes"] <= ARENA_BYTES
    assert len(ordinary.receipts) == 1, ordinary.receipts
    assert_route(legacy, streaming=False, count=SMALL_COUNT)
    assert_route(auto, streaming=certified, count=SMALL_COUNT)


@pytest.mark.parametrize("certified", [True, False], ids=["certified", "uncertified"])
def test_default_documents_are_identical_to_legacy_and_project_ordinary(tmp_path, certified):
    check_documents(tmp_path, certified=certified, jailed=False)


@pytest.mark.parametrize("certified", [True, False], ids=["certified", "uncertified"])
def test_jailed_documents_are_identical_to_legacy_and_project_ordinary(tmp_path, certified):
    check_documents(tmp_path, certified=certified, jailed=True)


def test_peak_rss_saving_requires_the_certified_route(tmp_path):
    assert sys.platform.startswith("linux"), "peak RSS acceptance requires Linux"
    modules = {name: native_module(certified) for name, certified in
               (("certified", True), ("uncertified", False))}
    tapes = {count: write_tape(tmp_path / f"bars-{count}.bin", count)
             for count in RSS_COUNTS}
    samples = {name: {mode: {count: [] for count in RSS_COUNTS}
                      for mode in ("auto", "legacy")} for name in modules}
    observations, populations = [], set()
    for repeat in range(RSS_REPEATS):
        # Alternate mode order and interleave the control to reduce ordering
        # bias. Each peak belongs to a fresh process, not a cumulative maximum.
        for count in RSS_COUNTS:
            for name, module in modules.items():
                modes = ("auto", "legacy") if repeat % 2 == 0 else ("legacy", "auto")
                runs = {}
                for mode in modes:
                    option = "--metrics-only" if mode == "auto" else "--metrics-only=legacy"
                    run = run_cli(tmp_path, module, tapes[count], option=option)
                    assert_nontrivial(run.document, count)
                    samples[name][mode][count].append(run.peak_bytes)
                    populations.add(run.document["summary"]["total_trades"])
                    observations.append({
                        "module": name, "mode": mode, "count": count, "repeat": repeat,
                        "peak_bytes": run.peak_bytes, "receipts": run.receipts,
                    })
                    runs[mode] = run
                assert runs["auto"].stdout == runs["legacy"].stdout
    assert len(populations) == 1, populations
    small, large = RSS_COUNTS
    slopes = {
        name: {mode: (statistics.median(by_count[large])
                      - statistics.median(by_count[small])) / (large - small)
               for mode, by_count in by_mode.items()}
        for name, by_mode in samples.items()}
    savings = {name: by_mode["legacy"] - by_mode["auto"]
               for name, by_mode in slopes.items()}
    evidence = {
        "counts": RSS_COUNTS, "repeats": RSS_REPEATS,
        "samples": observations, "growth_bytes_per_bar": slopes,
        "saving_bytes_per_bar": savings,
    }
    artifact = tmp_path / "rss-measurements.json"
    artifact.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"RSS measurements: {artifact}\n{json.dumps(evidence, sort_keys=True)}", flush=True)
    assert all(slope > 0 for by_mode in slopes.values() for slope in by_mode.values()), evidence
    assert savings["certified"] >= 12., evidence
    assert abs(savings["uncertified"]) < 4., evidence
    # Check actual routes after the RSS inequalities so a disabled capability
    # cannot masquerade as a smaller, unexecuted memory test.
    for observation in observations:
        run = Run(b"", {}, observation["receipts"], observation["peak_bytes"])
        assert_route(
            run, streaming=(observation["module"] == "certified"
                            and observation["mode"] == "auto"),
            count=observation["count"])
