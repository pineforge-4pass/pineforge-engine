"""Execute pinned main and this tree on the same OFF requests, on Linux.

CTest supplies PINEFORGE_REPORT_OFF_MODULE_MAP: an ordered TSV containing
probe<TAB>absolute CMake target filename, one row per corpus parity probe.
PINEFORGE_REPORT_OFF_DEFAULT_OHLCV names the fresh full-history 15m feed.
Missing history, modules, pinned inputs or fresh feeds are test failures.
No external expected-output artifact or capture step is used.
"""
from __future__ import annotations

from contextlib import ExitStack
from dataclasses import dataclass
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
# Pinned to main at the merge; when main moves again before this change merges, repoint all three to the new main head (and re-run the re-anchor and the OFF row).
BASE = "69cddc9d102d2ccf5605231cc508b82c08c7081c"
CORPUS_BASE = "8425ca820c4b7272db24d659a64d88e8e05b6407"
SUBSET = "scripts/corpus_parity_subset.txt"
SUBSET_SIZE = 54
ROUTES = ("full", "path-sink", "fd-sink")
CHANNEL_OPTIONS = {
    "--report-channel", "--terminal-status-fd", "--report-max-bytes",
    "--diagnostic-max-bytes", "--channel-wall-ms", "--channel-stall-ms",
    "--attempt-id", "--host-attestation-fd", "--test-only-host-attestation-fd",
}

# Only the child's clock observations are fixed. Its actual script entry point,
# _host_main, native execution, report writers and encoders remain in charge.
# The parent watchdog continues to use real time.
CLOCK_RUNNER = r"""
import json
from pathlib import Path
import runpy
import sys
import time

runner = Path(sys.argv[1])
arguments = json.loads(sys.argv[2])
for name, value in {
    "time": 1700000000.0, "time_ns": 1700000000000000000,
    "perf_counter": 1000.0, "perf_counter_ns": 1000000000000,
    "monotonic": 1000.0, "monotonic_ns": 1000000000000,
}.items():
    setattr(time, name, lambda value=value: value)
sys.path[:] = [str(runner.parent), *(path for path in sys.path if path)]
sys.argv = [str(runner), *arguments]
runpy.run_path(str(runner), run_name="__main__")
"""


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def git_bytes(*arguments, cwd=ROOT):
    result = subprocess.run(
        ["git", *arguments], cwd=cwd, capture_output=True, check=False)
    require(result.returncode == 0,
            f"required Git history is unavailable: {arguments!r}\n"
            + result.stderr.decode("utf-8", "replace"))
    return result.stdout


def write_reference(directory, sources):
    for relative, content in sources.items():
        path = directory / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    # Capabilities probe the installation's native planner beside docker/.
    # Give both scripts the same native helper availability.
    (directory / "lib").symlink_to(ROOT / "lib", target_is_directory=True)
    return directory / "docker/run_json.py"


@dataclass(frozen=True)
class Observation:
    argv: tuple[str, ...]
    exit_status: int
    stdout: bytes
    stderr: bytes
    sink: bytes | None

    def detail(self):
        return (f"exit={self.exit_status}; stdout[{len(self.stdout)}]="
                f"{self.stdout[:512]!r}; stderr[{len(self.stderr)}]="
                f"{self.stderr[:4096]!r}; sink_bytes="
                f"{None if self.sink is None else len(self.sink)}")


def invoke(runner, request, route, directory, environment):
    # Each side starts with an absent sink. The identical relative sink argument
    # also keeps the path embedded in the real summary report identical.
    directory.mkdir(parents=True, exist_ok=False)
    stdout_path, stderr_path = directory / "stdout", directory / "stderr"
    sink_path = directory / "report.sink"
    argv = list(request)
    process = None
    try:
        with ExitStack() as stack:
            stdout = stack.enter_context(stdout_path.open("xb"))
            stderr = stack.enter_context(stderr_path.open("xb"))
            sink_reader = None
            inherited_writer = None
            inherited = ()
            if route == "path-sink":
                argv += ["--report-sink", "report.sink"]
            elif route == "fd-sink":
                reader, writer = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
                sink_reader = stack.enter_context(reader)
                stack.enter_context(writer)
                # Allocate above the arena's reserved descriptor without
                # assuming the numbers of this process's existing descriptors.
                fd = fcntl.fcntl(writer.fileno(), fcntl.F_DUPFD_CLOEXEC, 64)
                inherited_writer = stack.enter_context(os.fdopen(fd, "wb", buffering=0))
                inherited = (fd,)
                argv += ["--report-sink-fd", str(fd)]
            require(not any(token.partition("=")[0] in CHANNEL_OPTIONS for token in argv),
                    f"OFF request contains a channel control: {argv!r}")
            require(not any(name.startswith("PINEFORGE_") for name in environment),
                    "OFF children must receive no PineForge environment controls")
            try:
                process = subprocess.Popen(
                    [sys.executable, "-B", "-s", "-c", CLOCK_RUNNER,
                     str(runner), json.dumps(argv)],
                    cwd=directory, env=environment, stdin=subprocess.PIPE,
                    stdout=stdout, stderr=stderr, pass_fds=inherited)
            finally:
                if inherited_writer is not None:
                    inherited_writer.close()
                if sink_reader is not None:
                    writer.close()
            process.stdin.close()
            if sink_reader is not None:
                with sink_path.open("xb") as sink_output:
                    while True:
                        chunk = sink_reader.recv(65536)
                        if not chunk:
                            break
                        sink_output.write(chunk)
            process.wait()
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
    return Observation(
        tuple(argv), process.returncode, stdout_path.read_bytes(), stderr_path.read_bytes(),
        sink_path.read_bytes() if sink_path.is_file() else None)


class OffIdentity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="report-off-identity-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        cls.runner = ROOT / "docker/run_json.py"
        paths = git_bytes("ls-tree", "-r", "--name-only", BASE, "--", "docker")
        # Every main-era sibling module and its data comes from Git, including
        # transitive imports. Neither PYTHONPATH nor the tree supplies a fallback.
        cls.sources = {
            name: git_bytes("show", f"{BASE}:{name}")
            for name in paths.decode("utf-8").splitlines()
            if Path(name).parent == Path("docker")
            and Path(name).suffix in (".py", ".json", ".def")
        }
        require("docker/run_json.py" in cls.sources, "main has no run_json.py")
        cls.reference = write_reference(cls.work / "main", cls.sources)
        require(cls.reference.resolve() != cls.runner.resolve(),
                "the reference and tree runners must be distinct files")
        subset = git_bytes("show", f"{BASE}:{SUBSET}")
        require((ROOT / SUBSET).read_bytes() == subset,
                "the corpus subset must be main's ordered list")
        cls.probes = [
            name for line in subset.decode("utf-8").splitlines()
            if (name := line.partition("#")[0].strip())
        ]
        require(len(cls.probes) == SUBSET_SIZE and len(set(cls.probes)) == SUBSET_SIZE,
                f"expected all {SUBSET_SIZE} distinct corpus probes")
        require(all(re.fullmatch(r"validation/[A-Za-z0-9][A-Za-z0-9._-]*", name)
                    for name in cls.probes), "invalid corpus probe name")
        map_path = os.environ.get("PINEFORGE_REPORT_OFF_MODULE_MAP")
        require(map_path, "CTest must supply PINEFORGE_REPORT_OFF_MODULE_MAP "
                "with one $<TARGET_FILE:strategy_...> per ordered subset probe")
        rows = [line.split("\t") for line in Path(map_path).read_text(
            encoding="utf-8").splitlines()]
        require(all(len(row) == 2 for row in rows), "invalid CMake module-map row")
        require([row[0] for row in rows] == cls.probes,
                "CMake module map must contain the whole subset, in order")
        cls.modules = {name: Path(path) for name, path in rows}
        for probe, module in cls.modules.items():
            require(module.is_absolute() and module.suffix == ".so" and module.is_file(),
                    f"build the real corpus strategy target for {probe}: {module}")
        corpus = ROOT / "corpus"
        require(git_bytes("rev-parse", "HEAD", cwd=corpus).decode().strip() == CORPUS_BASE,
                "OFF identity requires the pinned corpus checkout")

        # These unchanged helpers resolve inputs only. They never supply report
        # bytes, run a strategy, materialize feeds or replace a runner function.
        for name in ("derive_corpus_feeds.py", "run_strategy.py"):
            require((ROOT / "scripts" / name).read_bytes()
                    == git_bytes("show", f"{BASE}:scripts/{name}"),
                    f"fixed corpus request helper differs from main: {name}")
        sys.path.insert(0, str(ROOT / "scripts"))
        import derive_corpus_feeds as derived
        import run_strategy as corpus_requests

        default_feed = os.environ.get("PINEFORGE_REPORT_OFF_DEFAULT_OHLCV")
        require(default_feed, "CTest must supply PINEFORGE_REPORT_OFF_DEFAULT_OHLCV")
        require(Path(default_feed).resolve() == derived.DERIVED_15M.resolve(),
                "the default corpus feed must be the pinned full-history 15m feed")
        require(derived.SOURCE_1M.is_file() and derived.SOURCE_1M.stat().st_size >= 1000000,
                "prepare the pinned full 1m corpus feed, including its LFS content")
        pointer = re.fullmatch(
            rb"version https://git-lfs.github.com/spec/v1\n"
            rb"oid sha256:([0-9a-f]{64})\nsize ([0-9]+)\n",
            git_bytes("show", f"{CORPUS_BASE}:data/{derived.SOURCE_1M.name}", cwd=corpus))
        require(pointer is not None, "the pinned corpus feed must have its Git LFS identity")
        source_digest = hashlib.sha256()
        with derived.SOURCE_1M.open("rb") as source_feed:
            for chunk in iter(lambda: source_feed.read(1048576), b""):
                source_digest.update(chunk)
        require(derived.SOURCE_1M.stat().st_size == int(pointer[2])
                and source_digest.hexdigest() == pointer[1].decode("ascii"),
                "the 1m feed content differs from the pinned corpus Git LFS object")
        require(derived._derived_cache_is_fresh(
            derived.SOURCE_1M, (derived.DERIVED_15M, derived.DERIVED_15M_WINDOW),
            derived.DERIVATION_STAMP),
            "prepare fresh derived feeds once before starting corpus consumers")
        tracked = set(git_bytes("ls-tree", "-r", "--name-only", CORPUS_BASE,
                                "--", "validation", cwd=corpus).decode().splitlines())
        cls.requests = []
        for index, probe in enumerate(cls.probes):
            probe_dir = corpus / probe
            source = probe_dir / "generated.cpp"
            require(source.is_file() and source.read_bytes()
                    == git_bytes("show", f"{CORPUS_BASE}:{probe}/generated.cpp", cwd=corpus),
                    f"corpus source is missing or differs from its pin: {source}")
            inputs_path = probe_dir / "inputs.json"
            if f"{probe}/inputs.json" in tracked:
                raw = git_bytes("show", f"{CORPUS_BASE}:{probe}/inputs.json", cwd=corpus)
                require(inputs_path.read_bytes() == raw,
                        f"corpus inputs differ from their pin: {inputs_path}")
                params = json.loads(raw)
            else:
                require(not inputs_path.exists(), f"unexpected corpus inputs: {inputs_path}")
                params = {}
            feed, knobs = corpus_requests.inputs_run_kwargs(
                params, probe_dir, Path(default_feed), env={})
            require(not any(knobs.get(key) for key in (
                "aux_security_ohlcv_csv", "native_security_feeds",
                "account_currency_fx_series")),
                f"{probe} requires a feed route outside the ordinary run-json CLI")
            require(feed.is_file(), f"pinned corpus feed is absent: {feed}")
            start = knobs.get("ohlcv_start_ms")
            if start is not None:
                trimmed = cls.work / f"feed-{index}.csv"
                with feed.open("rb") as source_feed, trimmed.open("xb") as target_feed:
                    target_feed.write(next(source_feed))
                    for line in source_feed:
                        if int(line.partition(b",")[0]) >= start:
                            target_feed.write(line)
                feed = trimmed
            inputs = {str(key): str(value) for key, value in params.items()
                      if not str(key).startswith("tv_")
                      and key not in corpus_requests._VALIDATION_META_KEYS}
            overrides = {str(key): str(value) for key, value
                         in (knobs.get("strategy_overrides") or {}).items()}
            distribution = knobs["magnifier_distribution"]
            volume_weighted = knobs["magnifier_volume_weighted"]
            if distribution.upper() == "VOLUME_WEIGHTED":
                distribution, volume_weighted = "ENDPOINTS", True
            request = [
                "--so", str(cls.modules[probe]), "--ohlcv", str(feed),
                "--generated-cpp", str(source),
                "--inputs", json.dumps(inputs), "--overrides", json.dumps(overrides),
                "--input-tf", knobs["input_tf"] or "",
                "--script-tf", knobs["script_tf"] or "",
                "--chart-tz", knobs["chart_timezone"],
                "--bar-magnifier", str(knobs["bar_magnifier"]).lower(),
                "--magnifier-samples", str(knobs["magnifier_samples"]),
                "--magnifier-dist", distribution.lower(),
            ]
            if volume_weighted:
                request.append("--magnifier-volume-weighted")
            syminfo = dict(knobs["syminfo_strings"] or {})
            for field in ("timezone", "session", "type", "mintick", "pointvalue"):
                value = knobs["syminfo_" + field]
                if value is not None:
                    syminfo[field] = value
            if knobs["syminfo_metadata"]:
                syminfo.update(knobs["syminfo_metadata"])
            if syminfo:
                syminfo_path = cls.work / f"syminfo-{index}.json"
                syminfo_path.write_text(json.dumps(syminfo), encoding="utf-8")
                request += ["--syminfo", str(syminfo_path)]
            cls.requests.append(tuple(request))
        cls.environment = {
            name: value for name, value in os.environ.items()
            if not name.startswith(("PINEFORGE_", "PYTHON"))
            and name not in ("LD_PRELOAD", "LD_AUDIT")
        }
        scratch = cls.work / "scratch"
        scratch.mkdir()
        cls.environment.update(TMPDIR=str(scratch), LC_ALL="C.UTF-8", LANG="C.UTF-8",
                               TZ="UTC", PYTHONHASHSEED="0")

    def assert_bytes_equal(self, channel, reference, actual):
        if reference == actual:
            return
        if reference is None or actual is None:
            self.fail(f"{channel} bytes differ: one sink is absent")
        offset = next((index for index, pair in enumerate(zip(reference, actual))
                       if pair[0] != pair[1]), min(len(reference), len(actual)))
        self.fail(
            f"{channel} bytes differ: main length={len(reference)}, "
            f"tree length={len(actual)}, first difference={offset}; "
            f"main={reference[offset:offset + 96]!r}; "
            f"tree={actual[offset:offset + 96]!r}")

    def assert_identical(self, reference, actual):
        detail = f"main: {reference.detail()}\ntree: {actual.detail()}"
        self.assertEqual(reference.argv, actual.argv, "requests differ\n" + detail)
        self.assertEqual(reference.exit_status, actual.exit_status, detail)
        self.assert_bytes_equal("stdout", reference.stdout, actual.stdout)
        self.assert_bytes_equal("stderr", reference.stderr, actual.stderr)
        self.assert_bytes_equal("sink", reference.sink, actual.sink)

    def compare(self, request, route, name):
        directory = self.work / name
        reference = invoke(self.reference, request, route, directory / "main", self.environment)
        actual = invoke(self.runner, request, route, directory / "tree", self.environment)
        self.assert_identical(reference, actual)
        return reference, actual

    def assert_success(self, result, route):
        self.assertEqual(result.exit_status, 0, result.detail())
        report = json.loads(result.stdout)
        self.assertEqual(report["engine"], "pineforge")
        self.assertNotIn("error", report)
        self.assertNotIn("code", report)
        self.assertGreater(report["input"]["bars"], 0)
        self.assertGreater(report["summary"]["bars_processed"], 0)
        if route == "full":
            self.assertIsInstance(report["trades"], list)
            self.assertTrue(report["equity_curve"], "FULL must include real equity output")
            self.assertEqual(report["summary"]["total_trades"], len(report["trades"]))
            self.assertIsNone(result.sink)
        else:
            self.assertNotIn("trades", report)
            self.assertNotIn("equity_curve", report)
            self.assertTrue(result.sink, "a sink request must write real native sink bytes")
            self.assertTrue(result.sink.endswith(b"\n"))
            header = json.loads(result.sink.partition(b"\n")[0])
            trailer = json.loads(result.sink.rsplit(b"\n", 2)[-2])
            self.assertEqual(header["schema"], "pf-report-sink")
            self.assertEqual(trailer["trailer"], "pf-report-sink")
            self.assertGreater(trailer["counts"]["E"], 0)
            self.assertEqual(trailer["counts"]["T"], report["summary"]["total_trades"])
            if route == "path-sink":
                self.assertEqual(report["report_sink"]["path"], "report.sink")
            else:
                self.assertNotIn("report_sink", report)

    def test_off_identity_corpus_parity_subset(self):
        completed = []
        self.assertEqual(len(self.requests), SUBSET_SIZE)
        for index, (probe, request) in enumerate(zip(self.probes, self.requests)):
            for route in ROUTES:
                with self.subTest(probe=probe, route=route):
                    reference, actual = self.compare(request, route, f"probe-{index}-{route}")
                    self.assert_success(reference, route)
                    self.assert_success(actual, route)
                    completed.append((probe, route))
        self.assertEqual(completed, [(probe, route) for probe in self.probes for route in ROUTES],
                         "every probe must finish all three real report routes")

    def test_off_identity_capabilities_and_failures(self):
        module = str(self.modules[self.probes[0]])
        cases = (
            ("capabilities", ("--so", module, "--capabilities-json"), 0, None),
            ("parse-failure", (*self.requests[0], "--magnifier-samples", "not-an-integer"),
             2, "run_request_invalid"),
            ("request-refusal", (*self.requests[0], "--report-mode", "SUMMARY_ONLY"),
             1, "report_mode_unsupported"),
        )
        for name, request, status, code in cases:
            with self.subTest(scenario=name):
                reference, actual = self.compare(request, "full", name)
                for result in (reference, actual):
                    self.assertEqual(result.exit_status, status, result.detail())
                    report = json.loads(result.stdout)
                    self.assertEqual(report["engine"], "pineforge")
                    self.assertEqual(report.get("code"), code)
                    self.assertIsNone(result.sink)
                    if code is None:
                        self.assertIsInstance(report["capabilities"], dict)
                        self.assertTrue(report["capabilities"])
                        self.assertNotIn("error", report)
                    else:
                        self.assertIn("error", report)
                        if name == "parse-failure":
                            self.assertTrue(result.stderr, "parse failure must exercise stderr")

    def test_off_identity_negative_control(self):
        sources = dict(self.sources)
        needle = b'\nif __name__ == "__main__":\n'
        original = sources["docker/run_json.py"]
        self.assertEqual(original.count(needle), 1, "negative control needs the real script entry")
        marker = b"OFF identity negative control\n"
        sources["docker/run_json.py"] = original.replace(
            needle, needle + b'    sys.stdout.write("OFF identity negative control\\n")\n'
            b"    sys.stdout.flush()\n", 1)
        altered = write_reference(self.work / "altered-main", sources)
        reference, actual = self.compare(self.requests[0], "full", "negative-control-real")
        self.assert_success(reference, "full")
        self.assert_success(actual, "full")
        doctored = invoke(altered, self.requests[0], "full",
                          self.work / "negative-control-altered", self.environment)
        self.assertEqual(doctored.exit_status, 0, doctored.detail())
        self.assertEqual(doctored.stderr, reference.stderr)
        self.assertEqual(doctored.stdout, marker + reference.stdout)
        with self.assertRaisesRegex(AssertionError, "stdout bytes differ"):
            self.assert_identical(doctored, actual)

    def test_off_identity_help_and_legacy_abbreviation(self):
        reference, actual = self.compare(("--help",), "full", "legacy-help")
        for result in (reference, actual):
            self.assertEqual(result.exit_status, 0, result.detail())
            self.assertTrue(result.stdout.startswith(b"usage:"), result.detail())
            self.assertIn(b"--magnifier-samples", result.stdout)
            self.assertEqual(result.stderr, b"")
            self.assertIsNone(result.sink)
        # This prefix identifies exactly one existing main-era argparse option.
        request = (*self.requests[0], "--magnifier-sam", "3")
        reference, actual = self.compare(request, "full", "legacy-abbreviation")
        self.assert_success(reference, "full")
        self.assert_success(actual, "full")


if __name__ == "__main__":
    unittest.main(verbosity=2)
