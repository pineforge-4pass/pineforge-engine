"""Exercise the actual subset shell driver without a corpus, build or engine.

The fixture copies the driver and real canary checker into a minimal tree.
Read-only git queries, feed derivation, probe runs and byte identity use the
dispatcher at the bottom of this file; unknown commands are refused. Expected
CSV/sink bytes and the probe order are fixture inputs, never harvested output.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "scripts" / "check_corpus_parity.sh"
CHECKER = ROOT / "scripts" / "check_sink_canary_log.py"
PROBE_ORDER = ["zeta", "alpha", "omega", "beta", "middle", "last", "extra"]
STALE_LOG = "previous invocation must survive until the sink-on pass\n"


def csv_bytes(probe):
    return f"probe,entry,exit\n{probe},1.250000,1.500000\n".encode()


def sink_bytes(probe):
    return b"fixture sink bytes\x00\xff\n" + probe.encode() + b"\n"


def subset_fixture(tmp_path, *, count=7, custom_log=False, fault=""):
    root = tmp_path / "subset"
    scripts = root / "scripts"
    scripts.mkdir(parents=True)
    shutil.copy2(DRIVER, scripts / DRIVER.name)
    shutil.copy2(CHECKER, scripts / CHECKER.name)
    probes = PROBE_ORDER[:count]
    config = {
        "probes": probes, "fault": fault,
        "trades": ["entry/exit pair"], "equity": ["start", "middle", "end"],
    }
    (root / "fixture.json").write_text(json.dumps(config), encoding="utf-8")
    corpus = root / "corpus"
    corpus.mkdir()
    (corpus / "CMakeLists.txt").write_text("# fixture only\n", encoding="utf-8")
    for probe in probes:
        directory = corpus / "validation" / probe
        directory.mkdir(parents=True)
        (directory / "generated.cpp").write_text("// never compiled\n", encoding="utf-8")
        (directory / "strategy.so").write_bytes(b"never loaded")
        (directory / "engine_trades.csv").write_bytes(csv_bytes(probe))
    subset = scripts / "fixture-subset.txt"
    subset.write_text(
        "# fixture sweep order, deliberately unsorted\n" + "".join(
            f"validation/{probe} # selected\n" for probe in probes), encoding="utf-8")
    # These are synthetic fixture pins; the repository's frozen pins are unused.
    (scripts / "corpus_parity_baseline.txt").write_text(json.dumps({
        probe: hashlib.sha256(csv_bytes(probe)).hexdigest() for probe in probes,
    }), encoding="utf-8")
    (root / "events").mkdir()
    build = root / "build-subset"
    build.mkdir()
    log = root / "caller-canary.log" if custom_log else build / "sink_canary.log"
    log.write_text(STALE_LOG, encoding="utf-8")
    binaries = root / "fixture-bin"
    binaries.mkdir()
    for name, prefix in (("git", "--fixture-git "), ("cmake", "--refuse-cmake ")):
        command = binaries / name
        command.write_text(
            '#!/bin/sh\nexec "$PF_SUBSET_TEST_PYTHON" "$PF_SUBSET_TEST_SOURCE" '
            + prefix + '"$@"\n', encoding="utf-8")
        command.chmod(0o755)
    runner = scripts / "fixture-python"
    runner.write_text(
        '#!/bin/sh\nexec "$PF_SUBSET_TEST_PYTHON" "$PF_SUBSET_TEST_SOURCE" "$@"\n',
        encoding="utf-8")
    runner.chmod(0o755)
    env = os.environ.copy()
    for name in ("PINEFORGE_REPORT_SINK", "PINEFORGE_KEEP_REPORT_SINK",
                 "PINEFORGE_CANARY_LOG", "PINEFORGE_SUBSET_SINK", "SKIP_RUN",
                 "SKIP_BUILD", "BUILD_DIR", "SUBSET_FILE", "PYTHON"):
        env.pop(name, None)
    env.update({
        "PATH": str(binaries) + os.pathsep + env.get("PATH", ""),
        "PYTHON": str(runner), "SKIP_BUILD": "1", "JOBS": "2",
        "BUILD_DIR": "build-subset", "SUBSET_FILE": "scripts/fixture-subset.txt",
        "DIFF_FILES": "3", "DIFF_LINES": "7",
        "PF_SUBSET_TEST_PYTHON": sys.executable,
        "PF_SUBSET_TEST_SOURCE": str(Path(__file__).resolve()),
        "PF_SUBSET_TEST_ROOT": str(root),
        "PF_SUBSET_TEST_LOG": str(log),
    })
    if custom_log:
        env["PINEFORGE_CANARY_LOG"] = str(log)
    return root, env, probes, log


def run_subset(root, env, *args):
    return subprocess.run(
        ["bash", str(root / "scripts" / DRIVER.name), *(args or ("--subset",))],
        cwd=root, env=env, capture_output=True, text=True, check=False,
    )


def events(root):
    return [json.loads(path.read_text(encoding="utf-8"))
            for path in sorted((root / "events").glob("*.json"))]


def identities(root):
    path = root / "identity-calls.jsonl"
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


@pytest.mark.parametrize("count", [2, len(PROBE_ORDER)])
@pytest.mark.parametrize("custom_log", [False, True])
def test_subset_defaults_to_two_passes_and_reports_exact_artifacts(
        tmp_path, count, custom_log):
    root, env, probes, log = subset_fixture(
        tmp_path, count=count, custom_log=custom_log)
    if custom_log:
        env.update({"PINEFORGE_REPORT_SINK": "1", "PINEFORGE_KEEP_REPORT_SINK": "all"})
    result = run_subset(root, env)
    assert result.returncode == 0, result.stdout + result.stderr
    runs = events(root)
    assert {(row["phase"], row["probe"]) for row in runs} == {
        (phase, probe) for phase in ("0", "1") for probe in probes,
    }
    assert len(runs) == 2 * len(probes)
    kept = probes[:5]
    kept_dir = root / "build-subset" / "sink-kept"
    for row in runs:
        assert row["args"] == [
            "scripts/run_strategy.py", f"corpus/validation/{row['probe']}",
            "--so-name", "strategy.so",
        ]
        if row["phase"] == "0":
            assert row["log_before"] == STALE_LOG
        else:
            assert row["keep"] == ",".join(kept)
            assert Path(row["tmpdir"]) == kept_dir.resolve()
            assert root / row["canary_log"] == log
            assert row["log_before"] == ""
    calls = identities(root)
    assert len(calls) == 2
    assert calls[0] == calls[1]
    assert calls[0]["args"] == [
        "scripts/corpus_trades_identity.py", "--corpus", "corpus",
        "--subset", "scripts/fixture-subset.txt", "--files", "3", "--lines", "7",
    ]
    lines = result.stdout.splitlines()
    assert (f"sink-on subset: {len(probes)} sink_canary ok lines for "
            f"{len(probes)} probes") in lines
    assert f"sink-on kept sinks: {len(kept)}" in lines
    assert [line for line in lines if line.startswith("sink_kept:")] == [
        f"sink_kept: probe={probe} sha256={hashlib.sha256(sink_bytes(probe)).hexdigest()}"
        f" bytes={len(sink_bytes(probe))}" for probe in kept
    ]
    assert {path.name for path in kept_dir.iterdir()} == {
        f"pineforge-report-sink-{probe}.sink" for probe in kept
    }
    for probe in kept:
        assert (kept_dir / f"pineforge-report-sink-{probe}.sink").read_bytes() == sink_bytes(probe)
    assert STALE_LOG not in log.read_text(encoding="utf-8")
    assert sum(line.startswith("sink_canary:") for line in
               log.read_text(encoding="utf-8").splitlines()) == len(probes)
    assert result.stdout.count("materializing derived corpus feeds") == 1


@pytest.mark.parametrize("fault, code, diagnostic", [
    ("no-canaries", 1, "sink canary coverage failed"),
    ("failed-canary", 1, "sink canary coverage failed"),
    ("duplicate-canary", 1, "sink canary coverage failed"),
    ("wrong-probe", 1, "sink canary coverage failed"),
    ("missing-equity", 1, "sink canary coverage failed"),
    ("missing-kept", 1, "kept sink coverage failed"),
    ("missing-kept-file", 1, "kept sink coverage failed"),
    ("run-failed", 2, "sink-on subset probe failed to run"),
    ("sink-drift", 1, "sink-on subset byte-identity DRIFTED"),
    ("sink-identity-unrunnable", 2, "sink-on byte-identity check could not run"),
])
def test_subset_fails_when_the_sink_pass_cannot_prove_parity(
        tmp_path, fault, code, diagnostic):
    root, env, probes, _log = subset_fixture(tmp_path, fault=fault)
    result = run_subset(root, env)
    assert result.returncode == code, result.stdout + result.stderr
    assert diagnostic in result.stderr
    assert len(events(root)) == 2 * len(probes)
    assert "TradingView parity holds" not in result.stdout
    if fault in {"no-canaries", "failed-canary", "duplicate-canary",
                 "wrong-probe", "missing-equity"}:
        assert "sink-on subset:" not in result.stdout
    if fault == "failed-canary":
        assert f"probe={probes[0]}: FAIL" in result.stderr


@pytest.mark.parametrize("fault, code", [
    ("array-drift", 1), ("array-identity-unrunnable", 2),
])
def test_first_identity_failure_stops_before_any_sink_run(tmp_path, fault, code):
    root, env, probes, log = subset_fixture(tmp_path, custom_log=True, fault=fault)
    result = run_subset(root, env)
    assert result.returncode == code, result.stdout + result.stderr
    assert {(row["phase"], row["probe"]) for row in events(root)} == {
        ("0", probe) for probe in probes,
    }
    assert len(identities(root)) == 1
    assert log.read_text(encoding="utf-8") == STALE_LOG
    assert not (root / "build-subset" / "sink-kept").exists()
    assert "sink-on subset:" not in result.stdout


def test_explicit_opt_out_keeps_the_first_pass_sink_off(tmp_path):
    root, env, probes, log = subset_fixture(tmp_path, custom_log=True)
    env.update({"PINEFORGE_SUBSET_SINK": "0", "PINEFORGE_REPORT_SINK": "1"})
    result = run_subset(root, env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert {(row["phase"], row["probe"]) for row in events(root)} == {
        ("0", probe) for probe in probes,
    }
    assert len(identities(root)) == 1
    assert log.read_text(encoding="utf-8") == STALE_LOG
    assert "skipped (PINEFORGE_SUBSET_SINK=0)" in result.stdout
    assert "sink-on kept sinks:" not in result.stdout


def test_skip_run_judges_existing_csvs_and_explains_the_skipped_pass(tmp_path):
    root, env, _probes, log = subset_fixture(tmp_path)
    env["SKIP_RUN"] = "1"
    result = run_subset(root, env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert events(root) == []
    assert len(identities(root)) == 1
    assert log.read_text(encoding="utf-8") == STALE_LOG
    assert "skipped (SKIP_RUN=1)" in result.stdout


def test_help_prints_the_complete_header_and_sink_cost(tmp_path):
    root, env, _probes, _log = subset_fixture(tmp_path)
    result = run_subset(root, env, "--help")
    header = []
    for line in DRIVER.read_text(encoding="utf-8").splitlines()[1:]:
        if not line.startswith("#"):
            break
        header.append(line)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "\n".join(header) + "\n"
    assert "PINEFORGE_SUBSET_SINK=0" in result.stdout
    assert "plus the sink-on pass of about the same length" in result.stdout
    assert "Exit codes: 0 parity holds, 1 parity drifted, 2 the check could not run." in result.stdout


@pytest.mark.parametrize("sink_mode", [None, "0", "1"])
def test_full_mode_leaves_the_sink_choice_and_canary_log_to_its_caller(tmp_path, sink_mode):
    root, env, _probes, log = subset_fixture(tmp_path, custom_log=True)
    if sink_mode is not None:
        env["PINEFORGE_REPORT_SINK"] = sink_mode
    env.update({
        "PINEFORGE_SUBSET_SINK": "1", "PINEFORGE_KEEP_REPORT_SINK": "caller-choice",
        "EXPECTED_VERIFY": "Verified fixture full sweep",
    })
    full = root / "scripts" / "run_corpus.sh"
    full.write_text(
        '#!/bin/sh\nexec "$PF_SUBSET_TEST_PYTHON" "$PF_SUBSET_TEST_SOURCE" --fixture-full\n',
        encoding="utf-8")
    full.chmod(0o755)
    result = subprocess.run(
        ["bash", str(root / "scripts" / DRIVER.name)], cwd=root, env=env,
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    delegated = json.loads((root / "full-event.json").read_text(encoding="utf-8"))
    assert delegated == {
        "sink": sink_mode, "keep": "caller-choice", "log": str(log),
    }
    assert events(root) == []
    assert len(identities(root)) == 1
    assert "--subset" not in identities(root)[0]["args"]
    assert log.read_text(encoding="utf-8") == STALE_LOG
    assert not (root / "build-subset" / "sink-kept").exists()
    assert "sink-on subset:" not in result.stdout


def fixture_command(argv):
    """A child-process stub, dispatched by small file-based shell launchers."""
    if argv[0] == "-I":
        os.execv(sys.executable, [sys.executable, *argv])
    root = Path(os.environ["PF_SUBSET_TEST_ROOT"])
    config = json.loads((root / "fixture.json").read_text(encoding="utf-8"))
    if argv[0] == "--fixture-git":
        args = argv[1:]
        if args in (["rev-parse", "HEAD:corpus"],
                    ["-C", "corpus", "rev-parse", "HEAD"]):
            print("a" * 40)
            return 0
        if args == ["-C", "corpus", "status", "--porcelain", "--", "validation"]:
            return 0
        raise AssertionError(f"unexpected git query: {args}")
    if argv == ["scripts/derive_corpus_feeds.py"]:
        return 0
    if argv == ["--fixture-full"]:
        (root / "full-event.json").write_text(json.dumps({
            "sink": os.environ.get("PINEFORGE_REPORT_SINK"),
            "keep": os.environ.get("PINEFORGE_KEEP_REPORT_SINK"),
            "log": os.environ.get("PINEFORGE_CANARY_LOG"),
        }), encoding="utf-8")
        return 0
    if argv == ["scripts/verify_corpus.py", "--all", "--quiet"]:
        print(os.environ["EXPECTED_VERIFY"])
        return 0
    if argv[0] == "scripts/corpus_trades_identity.py":
        calls = root / "identity-calls.jsonl"
        call_index = len(calls.read_text().splitlines()) if calls.exists() else 0
        pins = root / "scripts" / "corpus_parity_baseline.txt"
        with calls.open("a", encoding="utf-8") as output:
            output.write(json.dumps({
                "args": argv, "pins": hashlib.sha256(pins.read_bytes()).hexdigest(),
            }) + "\n")
        phase = "array" if call_index == 0 else "sink"
        if config["fault"] == f"{phase}-identity-unrunnable":
            return 2
        baseline = json.loads(pins.read_text(encoding="utf-8"))
        return int(any(
            hashlib.sha256((root / "corpus" / "validation" / probe /
                            "engine_trades.csv").read_bytes()).hexdigest() != baseline[probe]
            for probe in config["probes"]
        ))
    if argv[0] != "scripts/run_strategy.py":
        raise AssertionError(f"unexpected fixture command: {argv}")
    probe = Path(argv[1]).name
    phase = os.environ.get("PINEFORGE_REPORT_SINK", "0")
    log = Path(os.environ["PF_SUBSET_TEST_LOG"])
    event = {
        "probe": probe, "phase": phase, "args": argv,
        "keep": os.environ.get("PINEFORGE_KEEP_REPORT_SINK", ""),
        "tmpdir": os.environ.get("TMPDIR", ""),
        "canary_log": os.environ.get("PINEFORGE_CANARY_LOG", ""),
        "log_before": log.read_text(encoding="utf-8"),
    }
    (root / "events" / f"{phase}-{probe}.json").write_text(json.dumps(event), encoding="utf-8")
    fault = config["fault"] if probe == config["probes"][0] else ""
    csv = root / "corpus" / "validation" / probe / "engine_trades.csv"
    data = csv_bytes(probe)
    if (phase == "0" and fault == "array-drift") or (phase == "1" and fault == "sink-drift"):
        data += b"unexpected trade\n"
    csv.write_bytes(data)
    if phase == "0":
        return 0
    assert phase == "1"
    directory = Path(os.environ["TMPDIR"])
    assert directory.is_dir()
    kept = os.environ["PINEFORGE_KEEP_REPORT_SINK"].split(",")
    if probe in kept and fault != "missing-kept":
        path = directory / f"pineforge-report-sink-{probe}.sink"
        path.write_bytes(sink_bytes(probe))
        print(f"sink_kept: probe={probe} path={path}")
        if fault == "missing-kept-file":
            path.unlink()
    name = "unexpected-probe" if fault == "wrong-probe" else probe
    line = (f"sink_canary: probe={name} bytes={len(sink_bytes(probe))} "
            f"records={len(config['trades'])} trades={len(config['trades'])} "
            f"equity={len(config['equity'])} ")
    line += "FAIL fixture-rejected" if fault == "failed-canary" else "ok"
    if fault == "missing-equity":
        line = line.replace(f" equity={len(config['equity'])}", "")
    if config["fault"] != "no-canaries":
        print(line)
        if fault == "duplicate-canary":
            print(line)
    return int(fault == "run-failed")


if __name__ == "__main__":
    raise SystemExit(fixture_command(sys.argv[1:]))
