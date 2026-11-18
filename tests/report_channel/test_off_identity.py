"""Execute pinned main and this tree on the same OFF requests, on Linux.

CTest supplies PINEFORGE_REPORT_OFF_MODULE_MAP: an ordered TSV containing
probe<TAB>absolute CMake target filename, one row per corpus parity probe.
PINEFORGE_REPORT_OFF_DEFAULT_OHLCV names the fresh full-history 15m feed.
Missing history, modules, pinned inputs or fresh feeds are test failures.
No external expected-output artifact or capture step is used.
"""
from __future__ import annotations

from contextlib import ExitStack
import ctypes
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
from types import ModuleType
import unittest


ROOT = Path(__file__).resolve().parents[2]
# Immutable reference implementation and runtime/feed request-helper pin.
BASE = "69cddc9d102d2ccf5605231cc508b82c08c7081c"
CORPUS_BASE = "804ef5bc7ce17a561a06ba9a2ac5b464d0501d1b"
SUBSET = "scripts/corpus_parity_subset.txt"
SUBSET_SIZE = 54
ROUTES = ("full", "path-sink", "fd-sink")
UNKNOWN_INPUT = "__pineforge_off_unknown_input__"
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


def load_module_bytes(name, path, content):
    """Execute the supplied source bytes directly, without a bytecode fallback."""
    module = ModuleType(name)
    module.__file__ = str(path.resolve())
    previous = sys.modules.get(name)
    sys.modules[name] = module
    try:
        exec(compile(content, module.__file__, "exec"), module.__dict__)
    except BaseException:
        if previous is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = previous
        raise
    return module


def load_pinned_module(name, path, expected):
    content = path.read_bytes()
    require(content == expected, f"BASE69 helper bytes differ: {path}")
    # The guarded bytes themselves are compiled, rather than a second read or
    # an import that could reuse a checkout module or cached bytecode.
    return load_module_bytes(name, path, content)


def load_request_helpers(directory, corpus, default_feed):
    """Load genuine BASE69 helpers, then bind only their real filesystem roots."""
    paths = {}
    sources = {}
    for filename in ("derive_corpus_feeds.py", "run_strategy.py"):
        relative = f"scripts/{filename}"
        content = git_bytes("show", f"{BASE}:{relative}")
        path = directory / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(0o444)
        paths[filename], sources[filename] = path, content
    derived = load_pinned_module(
        "_report_off_base69_derived", paths["derive_corpus_feeds.py"],
        sources["derive_corpus_feeds.py"])
    derived.REPO_ROOT = ROOT.resolve()
    derived.SOURCE_1M = corpus / "data/ohlcv_ETH-USDT-USDT_1m.csv"
    derived.DERIVED_DIR = default_feed.parent
    derived.DERIVED_15M = default_feed
    derived.DERIVED_15M_WINDOW = (
        default_feed.parent / "ohlcv_ETH-USDT-USDT_15m_window.csv")
    derived.DERIVATION_STAMP = default_feed.parent / ".derive_corpus_feeds.version"

    # BASE69 imports this one sibling by its ordinary name. Scope that alias to
    # loading the pinned helper so a checkout import cannot supply its globals.
    previous = sys.modules.get("derive_corpus_feeds")
    sys.modules["derive_corpus_feeds"] = derived
    try:
        requests = load_pinned_module(
            "_report_off_base69_requests", paths["run_strategy.py"],
            sources["run_strategy.py"])
    finally:
        if previous is None:
            sys.modules.pop("derive_corpus_feeds", None)
        else:
            sys.modules["derive_corpus_feeds"] = previous
    requests.REPO_ROOT = ROOT.resolve()
    require(requests.DERIVED_15M == default_feed
            and requests.DERIVED_DIR == derived.DERIVED_DIR,
            "BASE69 request helpers must use the explicitly bound real feed paths")
    return derived, requests


def load_input_contract():
    # Only the shared corpus metadata classifier and strict receipt reader come
    # from this tree. Runtime/feed resolution above remains pinned to BASE69.
    path = ROOT / "scripts/run_strategy.py"
    previous = sys.path[:]
    try:
        sys.path.insert(0, str(path.parent))
        return load_module_bytes("_report_off_input_contract", path, path.read_bytes())
    finally:
        sys.path[:] = previous


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
        # tests/CMakeLists.txt generates this map in the build's tests/ directory.
        cls.build = Path(map_path).resolve().parent.parent
        rows = [line.split("\t") for line in Path(map_path).read_text(
            encoding="utf-8").splitlines()]
        require(all(len(row) == 2 for row in rows), "invalid CMake module-map row")
        require([row[0] for row in rows] == cls.probes,
                "CMake module map must contain the whole subset, in order")
        cls.modules = {name: Path(path) for name, path in rows}
        for probe, module in cls.modules.items():
            require(module.is_absolute() and module.suffix == ".so" and module.is_file(),
                    f"build the real corpus strategy target for {probe}: {module}")
        corpus = (ROOT / "corpus").resolve()
        require(git_bytes("rev-parse", "HEAD", cwd=corpus).decode().strip() == CORPUS_BASE,
                "OFF identity requires the pinned corpus checkout")

        default_feed = os.environ.get("PINEFORGE_REPORT_OFF_DEFAULT_OHLCV")
        require(default_feed, "CTest must supply PINEFORGE_REPORT_OFF_DEFAULT_OHLCV")
        default_feed = Path(default_feed).resolve()
        require(default_feed == corpus / "data/derived/ohlcv_ETH-USDT-USDT_15m.csv",
                "the default corpus feed must be the pinned full-history 15m feed")
        # One shared request is built for both real children. These helpers never
        # supply report bytes, materialize feeds or replace a runner function.
        derived, corpus_requests = load_request_helpers(
            cls.work / "request-base69", corpus, default_feed)
        cls.input_contract = load_input_contract()
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
        cls.declared_inputs = {}
        cls.native_run_kwargs = {}
        cls.classification_deltas = []
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
            # Read literal names from the actual module's descriptor receipt;
            # source variables are never treated as native setter aliases.
            strategy = corpus_requests.Strategy(cls.modules[probe])
            state = strategy.lib.strategy_create(b"{}")
            require(state, f"NEEDS a strategy handle to bind corpus inputs: {probe}")
            try:
                names = cls.input_contract.declared_pine_input_names(strategy.lib, state)
            finally:
                strategy.lib.strategy_free(state)
            cls.declared_inputs[probe] = names
            inputs = cls.input_contract.corpus_input_values(
                params, declared_input_names=names)
            # This is BASE69's static filter, using its own immutable key set.
            # The current classifier has a separate role and must explain every
            # request delta before either runner is given the shared request.
            base_inputs = {
                str(key): str(value) for key, value in params.items()
                if not str(key).startswith("tv_")
                and key not in corpus_requests._VALIDATION_META_KEYS
            }
            delta = {
                "probe": probe, "module": str(cls.modules[probe]),
                "module_sha256": hashlib.sha256(cls.modules[probe].read_bytes()).hexdigest(),
                "declared": sorted(names), "base69": base_inputs, "bound": inputs,
                "removed": sorted(base_inputs.keys() - inputs.keys()),
                "added": sorted(inputs.keys() - base_inputs.keys()),
                "changed": sorted(key for key in base_inputs.keys() & inputs.keys()
                                  if base_inputs[key] != inputs[key]),
                "expected_removed": sorted(
                    key for key in params
                    if key in {"notes", "expected_tier", "_comment"} and key not in names),
            }
            cls.classification_deltas.append(delta)
            print("off_input_binding: " + json.dumps(delta, sort_keys=True), flush=True)
            require(not delta["added"] and not delta["changed"]
                    and delta["removed"] == delta["expected_removed"],
                    f"unexpected BASE69/current input delta for {probe}: {delta}")
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
            cls.native_run_kwargs[probe] = dict(knobs)
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
        self.assertEqual([row["probe"] for row in self.classification_deltas], self.probes)

    def test_off_identity_receipts_and_input_call_order(self):
        # Use CTest's existing modules, pinned source/inputs and real receipts.
        # A generated declared-metadata fixture still NEEDS a bound codegen pair;
        # that separate obligation is recorded in REPORT.md.
        cache = self.build / "CMakeCache.txt"
        self.assertIn(f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\n", cache.read_text(),
                      "the native seam must use this tree's actual CMake build")
        annotations = {"notes": "annotation", "expected_tier": "annotation",
                       "_comment": "annotation"}
        candidates = []
        for probe, request in zip(self.probes, self.requests):
            inputs = json.loads(request[request.index("--inputs") + 1])
            names = self.declared_inputs[probe]
            if inputs and inputs.keys() <= names and not annotations.keys() & names:
                candidates.append((probe, request, inputs))
        self.assertTrue(candidates,
                        "NEEDS an existing OFF module with configured declared inputs "
                        "and undeclared notes/expected_tier/_comment for native controls")
        probe, request, inputs = candidates[0]
        strategy = self.input_contract.Strategy(self.modules[probe])
        library = strategy.lib
        state = library.strategy_create(b"{}")
        self.assertTrue(state)
        try:
            names = self.input_contract.declared_pine_input_names(library, state)
            self.assertEqual(names, self.declared_inputs[probe])
            setter = library.strategy_set_input_checked
            setter.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                ctypes.c_void_p, ctypes.c_size_t]
            setter.restype = ctypes.c_int
            getter = library.strategy_get_effective_settings

            def effective_inputs():
                error = ctypes.create_string_buffer(1024)
                required = ctypes.c_size_t()
                self.assertEqual(getter(
                    state, None, 0, ctypes.byref(required), error, len(error)), 4)
                self.assertGreater(required.value, 0)
                receipt = ctypes.create_string_buffer(required.value)
                self.assertEqual(getter(
                    state, receipt, len(receipt), ctypes.byref(required), error, len(error)), 0)
                self.assertEqual(required.value, len(receipt.value) + 1)
                rows = json.loads(receipt.value)["inputs"]
                values = {row["name"]: row["effective_value"] for row in rows}
                self.assertEqual(frozenset(values), names)
                self.assertTrue(all(isinstance(value, str) for value in values.values()))
                return values

            # Apply only real pinned inputs, then round-trip their real
            # canonical receipt values. No guessed input title/type/value.
            for key, value in inputs.items():
                error = ctypes.create_string_buffer(1024)
                self.assertEqual(setter(
                    state, key.encode(), value.encode(), error, len(error)), 0,
                    error.value.decode("utf-8", "replace"))
            effective = effective_inputs()
            for key in inputs:
                error = ctypes.create_string_buffer(1024)
                self.assertEqual(setter(
                    state, key.encode(), effective[key].encode(), error, len(error)), 0,
                    error.value.decode("utf-8", "replace"))
            self.assertEqual(effective_inputs(), effective)
        finally:
            library.strategy_free(state)

        annotated = {**inputs, **annotations}
        self.assertNotEqual(annotated, inputs)
        bound = self.input_contract.corpus_input_values(annotated, declared_input_names=names)
        self.assertEqual(bound, inputs)
        # Forward every observed call to its actual native function.
        events = []
        observed = {}
        for name, event in (
                ("strategy_get_effective_settings", "receipt"),
                ("strategy_set_input", "input"), ("run_backtest_full", "bars")):
            native = getattr(library, name)

            def forward(*args, native=native, event=event):
                events.append(event)
                return native(*args)

            observed[name] = forward

        class ObservedLibrary:
            def __getattr__(self, name):
                return observed[name] if name in observed else getattr(library, name)

        strategy.lib = ObservedLibrary()
        feed = Path(request[request.index("--ohlcv") + 1])
        report = strategy.run(feed, params=annotated, **self.native_run_kwargs[probe])
        self.assertEqual(events, ["receipt", "receipt"] + ["input"] * len(inputs) + ["bars"])
        self.assertEqual(report["applied_inputs"], inputs)
        self.assertGreater(report["input_bars_processed"], 0)

    def test_off_identity_capabilities_and_failures(self):
        module = str(self.modules[self.probes[0]])
        self.assertNotIn(UNKNOWN_INPUT, self.declared_inputs[self.probes[0]])
        unknown = self.input_contract.corpus_input_values(
            {UNKNOWN_INPUT: "1"},
            declared_input_names=self.declared_inputs[self.probes[0]])
        self.assertEqual(unknown, {UNKNOWN_INPUT: "1"})
        cases = (
            ("capabilities", ("--so", module, "--capabilities-json"), 0, None),
            ("parse-failure", (*self.requests[0], "--magnifier-samples", "not-an-integer"),
             2, "run_request_invalid"),
            ("request-refusal", (*self.requests[0], "--report-mode", "SUMMARY_ONLY"),
             1, "report_mode_unsupported"),
            ("unknown-input", (*self.requests[0], "--inputs", json.dumps(unknown)),
             1, "setting_rejected"),
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
                        if name == "unknown-input":
                            self.assertEqual(report["args"], {
                                "entrypoint": "strategy_set_input", "reason": "unknown_key"})
                            self.assertIn("unknown input key", report["error"])

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
