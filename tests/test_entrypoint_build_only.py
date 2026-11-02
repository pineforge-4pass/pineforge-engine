"""Exercise persistent builds with observable compiler and engine processes."""

import ast
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRYPOINT = ROOT / "docker/entrypoint.sh"
BASE_ENTRYPOINT = Path(__file__).resolve().parent / "fixtures" / "entrypoint_base.sh"
BASE_SHA256 = "f274653d1278b2d31865a78f4f5708e0b9067f33b6e681f23265aaac95c4f007"
CPP = b"// supplied translation unit\nint marker = 7;\n"
PINE = "//@version=6\nstrategy('manifest')\n"
REPORT = b'{"engine":"pineforge","marker":"executed"}\n'


def channel_sets():
    tree = ast.parse((ROOT / "docker/run_json.py").read_text(encoding="utf-8"))
    host = next(node for node in tree.body
                if isinstance(node, ast.FunctionDef) and node.name == "_host_main")
    literals = {
        target.id: set(ast.literal_eval(node.value))
        for node in ast.walk(host) if isinstance(node, ast.Assign)
        for target in node.targets
        if isinstance(target, ast.Name) and target.id in ("channel_flags", "channel_environment")
    }
    branch = next(node for node in host.body if isinstance(node, ast.If))
    producer = {
        node.args[0].value
        for part in (branch.test, *branch.body) for node in ast.walk(part)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
        and ast.unparse(node.func.value) == "os.environ"
        and node.func.attr in ("get", "pop") and node.args
        and isinstance(node.args[0], ast.Constant)
    }
    return literals["channel_environment"], producer, literals["channel_flags"]


SPAWNING, PRODUCER, CHANNEL_FLAGS = channel_sets()
ENGINE_ENVIRONMENT = (
    "PINEFORGE_INPUTS", "PINEFORGE_OVERRIDES", "PINEFORGE_RUN_ID",
    "PINEFORGE_REPORT_SINK_FD", "PINEFORGE_INPUT_TF", "PINEFORGE_SCRIPT_TF",
    "PINEFORGE_BAR_MAGNIFIER", "PINEFORGE_MAGNIFIER_SAMPLES", "PINEFORGE_MAGNIFIER_DIST",
    "PINEFORGE_MAGNIFIER_VOLUME_WEIGHTED", "PINEFORGE_SYMINFO", "PINEFORGE_SYMBOL_FEEDS",
    "PINEFORGE_MAX_SYMBOL_FEEDS", "PINEFORGE_MAX_INVENTORY_BYTES", "PINEFORGE_TV_COMPAT",
    "PINEFORGE_TRADE_START_MS", "PINEFORGE_CHART_TZ", "PINEFORGE_BENCH",
    "PINEFORGE_WARMUP", "PINEFORGE_REPEATS", "PINEFORGE_REPORT_POLICY",
    "PINEFORGE_WINDOW_START_MS", "PINEFORGE_WINDOW_END_MS", "PINEFORGE_PREROLL_BARS",
    "PINEFORGE_FED_START_MS", "PINEFORGE_REQUEST_FEED_INVENTORY",
    "PINEFORGE_RUN_JSON_PROFILE", "PINEFORGE_COMMAND_SCRATCH_MAX_BYTES",
    "PINEFORGE_COMMAND_SCRATCH_DIR", "PF_SCRATCH_ARENA_FD", "PF_SCRATCH_ARENA_BYTES",
    "PF_SCRATCH_ARENA_JAILED", "PF_SCRATCH_WINDOW_BYTES", "TMPDIR", "PATH", "LANG", "LC_ALL",
    *sorted(SPAWNING | PRODUCER),
)
ALL_OPTIONS = {
    "PINEFORGE_INPUTS": '{"length":"8"}',
    "PINEFORGE_OVERRIDES": '{"initial_capital":"1000"}',
    "PINEFORGE_RUN_ID": 'run "é"\nnext',
    "PINEFORGE_REPORT_SINK_FD": "7",
    "PINEFORGE_INPUT_TF": "15",
    "PINEFORGE_SCRIPT_TF": "60",
    "PINEFORGE_BAR_MAGNIFIER": "true",
    "PINEFORGE_MAGNIFIER_SAMPLES": "6",
    "PINEFORGE_MAGNIFIER_DIST": "cosine",
    "PINEFORGE_MAGNIFIER_VOLUME_WEIGHTED": "true",
    "PINEFORGE_SYMINFO": "symbol metadata.json",
    "PINEFORGE_SYMBOL_FEEDS": "symbol feeds.json",
    "PINEFORGE_MAX_SYMBOL_FEEDS": "0",
    "PINEFORGE_MAX_INVENTORY_BYTES": "0",
    "PINEFORGE_TV_COMPAT": "true",
    "PINEFORGE_TRADE_START_MS": "60000",
    "PINEFORGE_CHART_TZ": "UTC",
    "PINEFORGE_BENCH": "true",
    "PINEFORGE_WARMUP": "1",
    "PINEFORGE_REPEATS": "2",
    "PINEFORGE_REPORT_POLICY": "selected-window/v1",
    "PINEFORGE_WINDOW_START_MS": "60000",
    "PINEFORGE_WINDOW_END_MS": "240000",
    "PINEFORGE_PREROLL_BARS": "1",
    "PINEFORGE_FED_START_MS": "0",
}

CODEGEN = '''
import hashlib
import os


def transpile(source, **kwargs):
    if os.environ.get("STUB_FAILURE") == "transpile":
        raise ValueError("source cannot be translated")
    return "// generated source\\nint source_size = %d;\\n" % len(source)


def transpile_with_request_inventory(source, **kwargs):
    cpp = transpile(source, **kwargs)
    return {"cpp": cpp, "request_feed_inventory": {
        "source_sha256": hashlib.sha256(cpp.encode()).hexdigest(),
        "artifact_sha256": None, "entries": []}}
'''
COMPILER = '''import os
from pathlib import Path
import sys

Path(os.environ["STUB_COMPILED"]).write_text("compiler entered\\n")
if os.environ.get("STUB_FAILURE") == "compile":
    raise SystemExit(17)
arguments = sys.argv[1:]
source = next(value for value in arguments if value.endswith(".cpp"))
Path(arguments[arguments.index("-o") + 1]).write_bytes(b"linked\\n" + Path(source).read_bytes())
'''
BINDER = '''import hashlib
import json
import os
from pathlib import Path
import sys

if os.environ.get("STUB_FAILURE") == "bind":
    raise SystemExit(19)
arguments = sys.argv[1:]
def value(flag):
    return arguments[arguments.index(flag) + 1]
document = json.loads(Path(value("--inventory")).read_bytes())
assert document["source_sha256"] == hashlib.sha256(Path(value("--cpp")).read_bytes()).hexdigest()
document["artifact_sha256"] = hashlib.sha256(Path(value("--so")).read_bytes()).hexdigest()
Path(value("--out")).write_text(json.dumps(document))
'''
RUNNER = '''import json
import os
import sys

names = {names!r}
document = {{"argv": [sys.executable, *sys.argv],
             "environment": {{name: os.environ[name] for name in names if name in os.environ}},
             "cwd": os.getcwd()}}
with open(os.environ["STUB_OBSERVED"], "a", encoding="ascii") as handle:
    handle.write(json.dumps(document, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\\n")
sys.stdout.write({report!r})
raise SystemExit(int(os.environ.get("STUB_ENGINE_STATUS", "0")))
'''


class Box:
    def __init__(self, root):
        self.root = root
        self.bin = root / "bin"
        self.indir = root / "in"
        self.prefix = root / "prefix"
        self.tmp = root / "tmp"
        self.work = root / "persistent é space"
        self.packages = root / "packages"
        self.compiled = root / "compiler-entered"
        self.observed = root / "engine-entered.jsonl"
        for directory in (self.bin, self.indir, self.prefix / "bin", self.prefix / "lib",
                          self.prefix / "include", self.tmp, self.work,
                          self.packages / "pineforge_codegen"):
            directory.mkdir(parents=True)
        (self.prefix / "lib/libpineforge.a").write_bytes(b"")
        (self.indir / "ohlcv.csv").write_text("timestamp,open,high,low,close,volume\n")
        (self.indir / "strategy.pine").write_text(PINE)
        (self.packages / "pineforge_codegen/__init__.py").write_text(CODEGEN)
        (self.packages / "pineforge_codegen/errors.py").write_text("class CompileError(Exception):\n    pass\n")
        (self.prefix / "bin/bind_compiled_inventory.py").write_text(BINDER)
        (self.prefix / "bin/run_json.py").write_text(
            RUNNER.format(names=ENGINE_ENVIRONMENT, report=REPORT.decode()))
        compiler = self.bin / "g++"
        compiler.write_text("#!" + sys.executable + "\n" + COMPILER)
        compiler.chmod(0o755)

    def env(self, extra=None):
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("PINEFORGE_", "PF_SCRATCH_", "STUB_", "PYTHON"))}
        environment.update({
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "PYTHONPATH": str(self.packages), "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONUTF8": "1", "TMPDIR": str(self.tmp),
            "PINEFORGE_PREFIX": str(self.prefix), "PINEFORGE_IN_DIR": str(self.indir),
            "STUB_COMPILED": str(self.compiled), "STUB_OBSERVED": str(self.observed),
        })
        environment.update(extra or {})
        return environment

    def run(self, extra=None, *, build=False, entrypoint=ENTRYPOINT):
        settings = dict(extra or {})
        if build:
            settings = {"PINEFORGE_BUILD_ONLY": "1", "PINEFORGE_WORK_DIR": str(self.work),
                        **settings}
        return subprocess.run(["bash", str(entrypoint)], cwd=self.root, env=self.env(settings),
                              stdin=subprocess.DEVNULL, capture_output=True, check=False)

    def manifest(self):
        return json.loads((self.work / "engine-step.json").read_bytes())

    def records(self):
        return [json.loads(line) for line in self.observed.read_bytes().splitlines()]

    def cpp_input(self):
        # Rename keeps the input bytes available while selecting the other source route.
        (self.indir / "strategy.pine").rename(self.indir / "source.pine")
        (self.indir / "strategy.cpp").write_bytes(CPP)
        return {"PINEFORGE_STRATEGY_CPP_SHA256": hashlib.sha256(CPP).hexdigest()}

    def supplied_inventory(self):
        path = self.root / "supplied.json"
        path.write_text(json.dumps({"source_sha256": hashlib.sha256(CPP).hexdigest(),
                                    "artifact_sha256": None, "entries": []}))
        return str(path)


def assert_build(result, box):
    # A completed build returns zero, emits no report, and retains the complete manifest.
    assert result.returncode == 0, result.stderr
    assert result.stdout == b""
    assert b"[pineforge] build complete; engine step not started\n" in result.stderr
    assert box.compiled.is_file()
    assert (box.work / "engine-step.json").is_file()
    assert (box.work / "strategy.so").is_file()


def assert_full_control(box, extra=None):
    result = box.run(extra)
    # The same executable must produce the report in full mode.
    assert result.returncode == 0, result.stderr
    assert result.stdout == REPORT
    assert box.observed.is_file()
    return result


def assert_refusal(result, box, option, names=()):
    # Entry-point request refusals are one JSON line, status four, before compilation.
    assert result.returncode == 4, result.stderr
    assert len(result.stdout.splitlines()) == 1
    document = json.loads(result.stdout)
    assert document["engine"] == "pineforge"
    assert document["code"] == "run_request_invalid"
    assert document["args"] == {"option": option}
    for name in names:
        assert name in document["error"]
    assert not box.compiled.exists()
    assert not box.observed.exists()


def direct(box, manifest, extra=None):
    environment = box.env({"PINEFORGE_BUILD_ONLY": "1",
                           "PINEFORGE_WORK_DIR": str(box.work), **(extra or {})})
    return subprocess.run(manifest["argv"], cwd=manifest["cwd"], env=environment,
                          stdin=subprocess.DEVNULL, capture_output=True, check=False)


@pytest.mark.parametrize("value", ["1", "true"])
def test_build_only_never_starts_the_engine_step(tmp_path, value):
    box = Box(tmp_path)
    assert_build(box.run({"PINEFORGE_BUILD_ONLY": value}, build=True), box)
    assert not box.observed.exists()
    # The persistent directory has exactly the three ordinary build artifacts.
    assert sorted(path.name for path in box.work.iterdir()) == [
        "engine-step.json", "strategy.cpp", "strategy.so"]
    assert_full_control(box)


def normalize_work(record, work):
    arguments = record["argv"]
    flags = ("--so", "--generated-cpp", "--request-feed-inventory")
    positions = {arguments.index(flag) + 1 for flag in flags if flag in arguments}
    # Every occurrence of this directory must be an artifact argument, with its exact suffix.
    occurrences = {index for index, value in enumerate(arguments) if work in value}
    assert occurrences == positions
    suffixes = {"--so": "/strategy.so", "--generated-cpp": "/strategy.cpp",
                "--request-feed-inventory": "/request_feed_inventory.json"}
    normalized = list(arguments)
    for flag in flags:
        if flag in arguments:
            index = arguments.index(flag) + 1
            assert arguments[index] == work + suffixes[flag]
            normalized[index] = "<work>" + suffixes[flag]
    return json.dumps({**record, "argv": normalized}, ensure_ascii=True,
                      sort_keys=True, separators=(",", ":")).encode("ascii")


@pytest.mark.parametrize("shape", ["pine", "cpp", "selected-pine", "selected-cpp"])
@pytest.mark.parametrize("options", ["unset", "empty", "all", "run-id-empty", "run-id-unset"])
def test_manifest_argv_is_the_full_run_argv(tmp_path, shape, options):
    box = Box(tmp_path)
    extra = box.cpp_input() if shape.endswith("cpp") else {}
    if options == "empty":
        extra.update({name: "" for name in ALL_OPTIONS})
    elif options in ("all", "run-id-empty", "run-id-unset"):
        extra.update(ALL_OPTIONS)
        if options == "run-id-empty":
            extra["PINEFORGE_RUN_ID"] = ""
        elif options == "run-id-unset":
            extra.pop("PINEFORGE_RUN_ID")
    if shape.startswith("selected"):
        extra["PINEFORGE_REPORT_POLICY"] = "selected-window/v1"
    if shape.endswith("cpp") and extra.get("PINEFORGE_REPORT_POLICY") == "selected-window/v1":
        extra["PINEFORGE_REQUEST_FEED_INVENTORY"] = box.supplied_inventory()
    assert_build(box.run(extra, build=True), box)
    assert not box.observed.exists()
    document = box.manifest()
    assert_full_control(box, extra)
    full = box.records()[0]
    result = direct(box, document, extra)
    # Both real process launches succeed; their own observations supply the expectation.
    assert result.returncode == 0, result.stderr
    assert result.stdout == REPORT
    phase = box.records()[1]
    full_work = str(Path(full["argv"][full["argv"].index("--so") + 1]).parent)
    assert full_work != str(box.work)
    assert phase["argv"] == document["argv"]
    assert document["cwd"] == str(box.root)
    assert normalize_work(full, full_work) == normalize_work(phase, str(box.work))
    assert not Path(full_work).exists()


@pytest.mark.parametrize("cause,status", [
    ("missing", 2), ("unvouched", 2), ("transpile", 5),
    ("compile", 3), ("bind", 3), ("guard", 4),
])
def test_pre_engine_exit_codes_match_the_full_run(tmp_path, cause, status):
    box = Box(tmp_path)
    extra = {}
    if cause == "missing":
        (box.indir / "strategy.pine").rename(box.indir / "source.pine")
    elif cause == "unvouched":
        box.cpp_input()
    elif cause == "guard":
        extra["PINEFORGE_COMPILE_AS_BYTES"] = ""
    else:
        extra["STUB_FAILURE"] = cause
        if cause == "bind":
            extra["PINEFORGE_REPORT_POLICY"] = "selected-window/v1"
    full = box.run(extra)
    built = box.run(extra, build=True)
    # Missing input and attestation map to two, translation to five, link/bind to three, usage to four.
    assert full.returncode == built.returncode == status
    assert full.stdout == built.stdout
    assert not box.observed.exists()
    assert not (box.work / "engine-step.json").exists()
    if cause == "missing":
        (box.indir / "source.pine").rename(box.indir / "strategy.pine")
    healthy = ({"PINEFORGE_STRATEGY_CPP_SHA256": hashlib.sha256(CPP).hexdigest()}
               if cause == "unvouched" else {})
    assert_full_control(box, healthy)
    # A fresh successful build makes the new mode itself part of this failure row.
    box.work = box.root / "successful-build"
    box.work.mkdir()
    assert_build(box.run(healthy, build=True), box)
    assert len(box.records()) == 1


@pytest.mark.parametrize("kind", [
    "unset", "empty", "relative", "file", "missing", "stale", "hidden", "off", "maybe",
])
def test_work_dir_contract(tmp_path, kind):
    box = Box(tmp_path)
    extra = {"PINEFORGE_BUILD_ONLY": "1", "PINEFORGE_WORK_DIR": str(box.work)}
    if kind == "unset":
        extra.pop("PINEFORGE_WORK_DIR")
    elif kind == "empty":
        extra["PINEFORGE_WORK_DIR"] = ""
    elif kind == "relative":
        extra["PINEFORGE_WORK_DIR"] = "relative"
    elif kind == "file":
        path = box.root / "file"
        path.write_bytes(b"unchanged")
        extra["PINEFORGE_WORK_DIR"] = str(path)
    elif kind == "missing":
        extra["PINEFORGE_WORK_DIR"] = str(box.root / "absent")
    elif kind == "stale":
        (box.work / "engine-step.json").write_bytes(b"old manifest\n")
        (box.work / "strategy.so").write_bytes(b"old library\n")
    elif kind == "hidden":
        (box.work / ".previous").write_bytes(b"old hidden file\n")
    elif kind == "off":
        extra.pop("PINEFORGE_BUILD_ONLY")
    elif kind == "maybe":
        extra["PINEFORGE_BUILD_ONLY"] = "maybe"
    before = {path.relative_to(box.root): None if path.is_dir() else path.read_bytes()
              for path in box.root.rglob("*")}
    assert_refusal(box.run(extra), box, "work_dir", ("PINEFORGE_WORK_DIR",))
    after = {path.relative_to(box.root): None if path.is_dir() else path.read_bytes()
             for path in box.root.rglob("*")}
    assert before == after
    assert not (box.root / "relative").exists()
    assert not (box.root / "absent").exists()
    assert_full_control(box)


@pytest.mark.parametrize("shape", ["pine", "cpp"])
def test_read_only_work_dir_is_refused_before_build(tmp_path, shape):
    box = Box(tmp_path)
    extra = box.cpp_input() if shape == "cpp" else {}
    box.work.chmod(0o555)
    try:
        # An unwritable directory is a request error (four), before either source path builds.
        assert_refusal(box.run(extra, build=True), box, "work_dir", ("PINEFORGE_WORK_DIR",))
        assert list(box.work.iterdir()) == []
    finally:
        box.work.chmod(0o755)
    assert_full_control(box, extra)


def test_symlink_work_dir_is_refused_before_build(tmp_path):
    box = Box(tmp_path)
    target = box.work
    box.work = box.root / "linked-work"
    box.work.symlink_to(target, target_is_directory=True)
    # A directory symlink is a request error (four), leaving its empty target untouched.
    assert_refusal(box.run(build=True), box, "work_dir", ("PINEFORGE_WORK_DIR",))
    assert box.work.is_symlink()
    assert list(target.iterdir()) == []
    assert_full_control(box)


@pytest.mark.parametrize("value", ["1", "true"])
def test_installed_entrypoint_builds_by_itself(tmp_path, value):
    box = Box(tmp_path)
    installed = box.prefix / "bin/entrypoint.sh"
    installed.write_bytes(ENTRYPOINT.read_bytes())
    # Installing only the shell script retains validation and publication, so the build succeeds.
    assert_build(box.run({"PINEFORGE_BUILD_ONLY": value}, build=True,
                         entrypoint=installed), box)
    assert not box.observed.exists()
    document = box.manifest()
    result = direct(box, document)
    # Executing the published argv starts the same engine process with its recorded cwd and argv.
    assert result.returncode == 0, result.stderr
    assert result.stdout == REPORT
    assert box.records()[0]["argv"] == document["argv"]
    assert box.records()[0]["cwd"] == document["cwd"]
    baseline = box.run()
    full = box.run(entrypoint=installed)
    # Without either variable both installations return zero with identical report and diagnostics.
    assert full.returncode == baseline.returncode == 0
    assert full.stdout == baseline.stdout == REPORT
    assert full.stderr == baseline.stderr
    assert len(box.records()) == 3


def test_manifest_hashes_are_of_the_files(tmp_path):
    box = Box(tmp_path)
    extra = {"PINEFORGE_REPORT_POLICY": "selected-window/v1", "PINEFORGE_REPORT_SINK_FD": "7"}
    assert_build(box.run(extra, build=True), box)
    assert not box.observed.exists()
    path = box.work / "engine-step.json"
    document = box.manifest()
    # The manifest is private, versioned, ASCII JSON and inherits the supplied sink descriptor.
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert path.read_bytes().isascii()
    assert document["schema"] == "pineforge-engine-step/v1"
    assert document["work_dir"] == str(box.work)
    assert document["inherits"] == {"environment": "unchanged", "descriptors": ["7"]}
    assert document["artifacts"]["ohlcv"] == {"path": str(box.indir / "ohlcv.csv"), "sha256": None}
    for name in ("so", "cpp", "request_feed_inventory"):
        item = document["artifacts"][name]
        artifact = Path(item["path"])
        assert item["sha256"] == hashlib.sha256(artifact.read_bytes()).hexdigest()
        artifact.write_bytes(artifact.read_bytes() + b"changed\n")
        assert item["sha256"] != hashlib.sha256(artifact.read_bytes()).hexdigest()
    assert_full_control(box, extra)


@pytest.fixture(scope="module")
def base_entrypoint_source():
    assert hashlib.sha256(BASE_ENTRYPOINT.read_bytes()).hexdigest() == BASE_SHA256, (
        "The stored base copy was altered")
    source = BASE_ENTRYPOINT.read_bytes()
    # The frozen script predates persistent builds and supplies an independent reference.
    assert source != ENTRYPOINT.read_bytes(), (
        "The stored base copy must differ from the installed entrypoint")
    return source


@pytest.mark.parametrize("mode", [None, "", "0", "false", "yes", "maybe"])
@pytest.mark.parametrize("engine_status", [0, 23])
def test_full_run_is_unchanged(tmp_path, mode, engine_status, base_entrypoint_source):
    box = Box(tmp_path)
    reference = box.root / "reference.sh"
    reference.write_bytes(base_entrypoint_source)
    extra = {"STUB_ENGINE_STATUS": str(engine_status)}
    if mode is not None:
        extra["PINEFORGE_BUILD_ONLY"] = mode
    baseline = box.run(extra, entrypoint=reference)
    actual = box.run(extra)
    # Bash maps any nonzero engine status to four and preserves the report and diagnostics.
    assert actual.returncode == baseline.returncode == (4 if engine_status else 0)
    assert actual.stdout == baseline.stdout == REPORT
    assert actual.stderr == baseline.stderr
    assert len(box.records()) == 2
    for record in box.records():
        work = Path(record["argv"][record["argv"].index("--so") + 1]).parent
        assert not work.exists()
    # The paired build stops before this same failing or successful executable.
    assert_build(box.run({"STUB_ENGINE_STATUS": str(engine_status)}, build=True), box)
    assert len(box.records()) == 2


def test_status_mapping_is_documented_not_hidden(tmp_path):
    box = Box(tmp_path)
    extra = {"STUB_ENGINE_STATUS": "23"}
    assert_build(box.run(extra, build=True), box)
    assert not box.observed.exists()
    document = box.manifest()
    mapping = document["status_mapping"]
    source = ENTRYPOINT.read_text(encoding="utf-8")
    failure = re.search(r"^readonly ENGINE_FAILURE_STATUS=(\d+)$", source, re.MULTILINE)
    assert failure is not None
    status = failure.group(1)
    exit_table = source.split("# Exit codes:\n", 1)[1].split("\n#\n", 1)[0]
    build_statuses = [int(value) for value in re.findall(
        r"^#\s+(\d+)\s+(?:compile|transpile) failure", exit_table, re.MULTILINE)]
    # The documented engine exits are zero, one and two; build exits come from the shell source.
    assert mapping["engine_step_statuses"] == [0, 1, 2]
    assert mapping["build_phase_only"] == build_statuses
    assert set(mapping["engine_step_meaning"]) == {
        str(value) for value in mapping["engine_step_statuses"]}
    entrypoint_statuses = re.findall(r"^#\s+(\d+)\s+", exit_table, re.MULTILINE)
    assert set(mapping["entrypoint_only_codes"]) == set(entrypoint_statuses) - {"0"}
    assert "full-run mapping" in mapping["entrypoint_only_codes"][status]
    assert mapping["full_run_mapping"] == (
        "the entrypoint reports any non-zero engine-step status as " + status)
    assert "128+N" in mapping["signals"]
    assert '"${ENGINE_FAILURE_STATUS}" "${ENGINE_ARGV[@]}"' in source
    assert 'exit "${ENGINE_FAILURE_STATUS}"' in source
    # The same stub retains its configured status directly and uses the shell constant in full mode.
    assert direct(box, document, extra).returncode == int(extra["STUB_ENGINE_STATUS"])
    assert box.observed.is_file()
    full = box.run(extra)
    assert full.returncode == int(status)
    assert b"[pineforge] backtest failed\n" in full.stderr


def test_status_lists_match_run_json(tmp_path):
    box = Box(tmp_path)
    assert_build(box.run(build=True), box)
    assert not box.observed.exists()
    mapping = box.manifest()["status_mapping"]
    engine_statuses = set(mapping["engine_step_statuses"])
    tree = ast.parse((ROOT / "docker/run_json.py").read_text(encoding="utf-8"))
    definitions = {node.name: node for node in tree.body
                   if isinstance(node, (ast.FunctionDef, ast.ClassDef))}
    scopes = {
        "main", "_host_main", "_main", "_csv2bin_main",
        "_write_report", "_capabilities_main", "_selected_run", "_ArgumentParser",
    }
    scopes.update(node.name for node in tree.body if isinstance(node, ast.ClassDef)
                  and any(isinstance(base, ast.Name) and base.id == "_ArgumentParser"
                          for base in node.bases))
    # parse_magnifier_dist returns distribution identifiers, never process exit statuses.
    scopes.discard("parse_magnifier_dist")
    observed = set()

    def record(value):
        if isinstance(value, ast.Constant) and type(value.value) is int:
            observed.add(value.value)
            assert value.value in engine_statuses, (value.lineno, value.value)
        elif isinstance(value, ast.IfExp):
            record(value.body)
            record(value.orelse)

    for name in sorted(scopes):
        for node in ast.walk(definitions[name]):
            if isinstance(node, ast.Return):
                record(node.value)
            elif isinstance(node, ast.Call):
                function = ast.unparse(node.func)
                if function in ("SystemExit", "sys.exit", "self.exit") and node.args:
                    record(node.args[0])
            elif isinstance(node, ast.Assign):
                if any(isinstance(target, ast.Name) and target.id == "status"
                       for target in node.targets):
                    record(node.value)
    # Typed failures propagate through main, so include their defaults and explicit status arguments.
    for name in ("RunFailure", "_request_invalid"):
        for node in ast.walk(definitions[name]):
            if isinstance(node, ast.FunctionDef):
                arguments = node.args
                defaults = list(zip(arguments.args[-len(arguments.defaults):], arguments.defaults))
                defaults.extend(zip(arguments.kwonlyargs, arguments.kw_defaults))
                for argument, value in defaults:
                    if argument.arg == "exit_status":
                        record(value)
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            for keyword in node.keywords:
                if keyword.arg == "exit_status":
                    record(keyword.value)
    # All declared engine statuses must occur in independently parsed process exit paths.
    assert observed == engine_statuses
    assert observed.isdisjoint(mapping["build_phase_only"])
    failure = re.search(r"^readonly ENGINE_FAILURE_STATUS=(\d+)$",
                        ENTRYPOINT.read_text(encoding="utf-8"), re.MULTILINE)
    assert failure is not None
    assert int(failure.group(1)) not in observed
    assert ast.unparse(tree.body[-1].body[0]) == "sys.exit(_host_main())"
    assert any(isinstance(node, ast.Assign) and ast.unparse(node) == "_REPORT_MAIN = main"
               for node in tree.body)

    failures = []
    for cause in ("compile", "transpile"):
        fault = Box(box.root / cause)
        result = fault.run({"STUB_FAILURE": cause}, build=True)
        failures.append(result.returncode)
        assert not fault.observed.exists()
        assert not (fault.work / "engine-step.json").exists()
    # Real shell failure branches supply the build-only list independently of manifest serialization.
    assert failures == mapping["build_phase_only"]

    readme = " ".join((ROOT / "docker/README.md").read_text(encoding="utf-8").split())
    documented_engine = re.search(r"The engine step returns (\d+), (\d+) or (\d+)", readme)
    documented_build = re.search(
        r"Statuses (\d+) \(compile or bind failure\) and (\d+) \(transpile failure\)", readme)
    documented_mapping = re.search(r"every nonzero engine status to (\d+)", readme)
    assert documented_engine is not None
    assert documented_build is not None
    assert documented_mapping is not None
    # Prose lists must match the independently checked manifest lists and shell mapping.
    assert list(map(int, documented_engine.groups())) == mapping["engine_step_statuses"]
    assert list(map(int, documented_build.groups())) == failures
    assert documented_mapping.group(1) == failure.group(1)
    assert mapping["full_run_mapping"].endswith(" " + documented_mapping.group(1))
    assert_full_control(box)


@pytest.mark.parametrize("build", ["1", "true"])
@pytest.mark.parametrize("transpile", ["1", "true"])
def test_build_only_and_transpile_only(tmp_path, build, transpile):
    box = Box(tmp_path)
    assert_refusal(box.run({"PINEFORGE_BUILD_ONLY": build,
                           "PINEFORGE_TRANSPILE_ONLY": transpile,
                           "PINEFORGE_WORK_DIR": str(box.work)}),
                   box, "arguments", ("PINEFORGE_BUILD_ONLY", "PINEFORGE_TRANSPILE_ONLY"))
    assert list(box.work.iterdir()) == []
    assert_full_control(box)


@pytest.mark.parametrize("value", ["0", "false", "", "yes"])
def test_false_mode_spellings_keep_their_original_paths(tmp_path, value):
    box = Box(tmp_path)
    assert_build(box.run({"PINEFORGE_TRANSPILE_ONLY": value}, build=True), box)
    assert not box.observed.exists()
    result = box.run({"PINEFORGE_BUILD_ONLY": value, "PINEFORGE_TRANSPILE_ONLY": "1"})
    # The 13-byte version line plus 21-byte strategy line yields a 34-byte source.
    assert result.returncode == 0
    assert result.stdout == b"// generated source\nint source_size = 34;\n"
    assert_full_control(box, {"PINEFORGE_BUILD_ONLY": value})
    refused = Box(box.root / "refused")
    assert_refusal(refused.run({"PINEFORGE_BUILD_ONLY": value,
                               "PINEFORGE_WORK_DIR": str(refused.work)}),
                   refused, "work_dir")
    assert_full_control(refused)


@pytest.mark.parametrize("name", sorted(SPAWNING | PRODUCER))
@pytest.mark.parametrize("value", ["", "enabled"])
def test_build_only_refuses_the_channel_names(tmp_path, name, value):
    box = Box(tmp_path)
    extra = {name: value}
    assert_refusal(box.run(extra, build=True), box, "arguments", (name,))
    assert list(box.work.iterdir()) == []
    assert_full_control(box, extra)


@pytest.mark.parametrize("name,value", [
    ("PINEFORGE_RUN_ID", "--attempt-id"),
    ("PINEFORGE_CHART_TZ", "--report-channel"),
    ("PINEFORGE_RUN_ID", "--attempt-id=other"),
    ("PINEFORGE_CHART_TZ", "--report-channel=supervised/v1"),
], ids=["run-id", "chart-tz", "run-id-equals", "chart-tz-equals"])
def test_build_only_refuses_channel_tokens_in_option_values(tmp_path, name, value):
    box = Box(tmp_path)
    result = box.run({name: value}, build=True)
    # A channel token in any argument value is one request error (four), with no runnable artifact.
    assert result.returncode == 4, result.stderr
    assert len(result.stdout.splitlines()) == 1
    document = json.loads(result.stdout)
    assert document["engine"] == "pineforge"
    assert document["code"] == "run_request_invalid"
    assert document["args"] == {"option": "arguments"}
    assert value in document["error"]
    assert box.compiled.is_file()
    assert not box.observed.exists()
    assert not (box.work / "engine-step.json").exists()
    assert not (box.work / "strategy.so").exists()
    assert list(box.work.glob("engine-step.*.json.tmp")) == []
    # The ordinary run-id control still builds and reaches the engine with its observed arguments.
    ordinary = Box(box.root / "ordinary")
    extra = {"PINEFORGE_RUN_ID": "ordinary-run"}
    assert_build(ordinary.run(extra, build=True), ordinary)
    assert direct(ordinary, ordinary.manifest(), extra).returncode == 0
    assert ordinary.records()[0]["argv"] == ordinary.manifest()["argv"]
    assert_full_control(box, {name: value})


@pytest.mark.parametrize("value", ["7", ""])
def test_report_sink_descriptor_is_accepted(tmp_path, value):
    box = Box(tmp_path)
    extra = {"PINEFORGE_REPORT_SINK_FD": value}
    assert_build(box.run(extra, build=True), box)
    assert not box.observed.exists()
    # Set descriptors are retained verbatim, including an empty value for runtime validation.
    assert box.manifest()["inherits"]["descriptors"] == [value]
    assert_full_control(box, extra)


@pytest.mark.parametrize("cause,status", [("compile", 3), ("transpile", 5)])
def test_failed_build_leaves_no_manifest(tmp_path, cause, status):
    box = Box(tmp_path)
    result = box.run({"STUB_FAILURE": cause}, build=True)
    # A failed link returns three; a failed translation returns five, without a published command.
    assert result.returncode == status
    assert not (box.work / "engine-step.json").exists()
    assert not box.observed.exists()
    assert_full_control(box)
    box.work = box.root / "fresh"
    box.work.mkdir()
    assert_build(box.run(build=True), box)
    assert Path(box.manifest()["argv"][0]).is_absolute()


def test_relative_python_resolution_is_refused(tmp_path):
    box = Box(tmp_path)
    interpreter = box.bin / "python3"
    interpreter.symlink_to(shutil.which("python3"))
    extra = {"PATH": "bin" + os.pathsep + os.environ["PATH"]}
    result = box.run(extra, build=True)
    # A relative executable resolution is an internal error, after a successful compilation.
    assert result.returncode == 4, result.stderr
    assert json.loads(result.stdout)["code"] == "harness_internal_error"
    assert box.compiled.exists()
    assert not (box.work / "engine-step.json").exists()
    assert not box.observed.exists()
    assert_full_control(box, extra)


def test_manifest_publication_failure_is_typed(tmp_path):
    box = Box(tmp_path)
    # The compiler makes the destination a directory, so atomic publication must fail.
    compiler = box.bin / "g++"
    compiler.write_text(compiler.read_text() + '\nPath(sys.argv[sys.argv.index("-o") + 1]).with_name("engine-step.json").mkdir()\n')
    result = box.run(build=True)
    assert result.returncode == 4
    document = json.loads(result.stdout)
    assert document["code"] == "harness_internal_error"
    assert document["args"] == {}
    assert box.compiled.exists()
    assert not (box.work / "engine-step.json").is_file()
    assert not box.observed.exists()
    # A failed replacement leaves zero private manifest publication files.
    assert list(box.work.glob("engine-step.*.json.tmp")) == []
    assert len(result.stdout.splitlines()) == 1
    assert_full_control(box)


def test_manifest_sync_failure_is_typed(tmp_path):
    box = Box(tmp_path)
    (box.packages / "sitecustomize.py").write_text(
        'import os\nimport sys\n'
        'if sys.argv[1:2] == ["manifest"]:\n'
        '    def fail_sync(fd):\n'
        '        raise OSError("manifest sync failed")\n'
        '    os.fsync = fail_sync\n')
    result = box.run(build=True)
    # A file sync failure returns four and publishes neither a manifest nor a temporary file.
    assert result.returncode == 4, result.stderr
    assert len(result.stdout.splitlines()) == 1
    document = json.loads(result.stdout)
    assert document["engine"] == "pineforge"
    assert document["code"] == "harness_internal_error"
    assert document["args"] == {}
    assert box.compiled.is_file()
    assert not box.observed.exists()
    assert not (box.work / "engine-step.json").exists()
    assert list(box.work.glob("engine-step.*.json.tmp")) == []
    assert_full_control(box)


def test_manifest_is_flushed_and_synced_before_publication(tmp_path):
    box = Box(tmp_path)
    synced = box.root / "synced.json"
    events = box.root / "publication-events"
    (box.packages / "sitecustomize.py").write_text(
        'import os\nfrom pathlib import Path\nimport sys\n'
        'if sys.argv[1:2] == ["manifest"]:\n'
        '    real_sync, real_replace = os.fsync, os.replace\n'
        '    def sync(fd):\n'
        '        real_sync(fd)\n'
        '        Path(os.environ["STUB_SYNCED"]).write_bytes(os.pread(fd, os.fstat(fd).st_size, 0))\n'
        '        with open(os.environ["STUB_PUBLICATION_EVENTS"], "a") as log:\n'
        '            log.write("sync\\n")\n'
        '    def replace(source, destination):\n'
        '        with open(os.environ["STUB_PUBLICATION_EVENTS"], "a") as log:\n'
        '            log.write("replace\\n")\n'
        '        real_replace(source, destination)\n'
        '    os.fsync, os.replace = sync, replace\n')
    extra = {"STUB_SYNCED": str(synced), "STUB_PUBLICATION_EVENTS": str(events)}
    assert_build(box.run(extra, build=True), box)
    # Publication follows one completed file sync, over all bytes including the final newline.
    assert events.read_bytes() == b"sync\nreplace\n"
    assert synced.read_bytes() == (box.work / "engine-step.json").read_bytes()
    assert synced.read_bytes().endswith(b"\n")
    assert json.loads(synced.read_bytes()) == box.manifest()
    assert list(box.work.glob("engine-step.*.json.tmp")) == []
    assert not box.observed.exists()
    assert_full_control(box, extra)


def test_forbidden_lists_match_run_json(tmp_path):
    box = Box(tmp_path)
    assert_build(box.run(ALL_OPTIONS, build=True), box)
    assert not box.observed.exists()
    document = box.manifest()
    # Dispatch has eleven spawning names, three producer names and twelve exact flags.
    assert len(SPAWNING) == 11
    assert len(PRODUCER) == 3
    assert len(CHANNEL_FLAGS) == 12
    assert set(document["forbidden_environment_spawning"]) == SPAWNING
    assert set(document["forbidden_environment_producer"]) == PRODUCER
    assert set(document["forbidden_flags"]) == CHANNEL_FLAGS
    assert not set(token.partition("=")[0] for token in document["argv"]) & CHANNEL_FLAGS
    for index, name in enumerate(sorted(SPAWNING | PRODUCER)):
        refusal = Box(box.root / str(index))
        assert_refusal(refusal.run({name: ""}, build=True), refusal, "arguments", (name,))
        assert_full_control(refusal)
    assert_full_control(box, ALL_OPTIONS)
