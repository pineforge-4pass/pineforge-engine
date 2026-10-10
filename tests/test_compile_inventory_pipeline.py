"""Tests of the trusted compile pipeline's request-feed inventory handling:
docker/entrypoint.sh on the selected-window path and docker/bind_compiled_inventory.py.

The entrypoint is driven as a process (bash) inside a private tree
that holds a stub g++, a stub run_json.py and a FAKE pineforge_codegen package on
PYTHONPATH. The fake mimics, from the interface text only, what these rows need of
transpile, transpile_with_request_inventory and bind_request_feed_inventory; it is
not the pinned code, and it does not reproduce the pinned shape check, so these
rows cannot show the pinned binder agrees.

Every stub appends to one ordered event log, so a row can assert what ran and what
did not: gxx (the compiler), helper (bind_compiled_inventory.py started), pinned:bind
(the pinned binder was called), run_json (the harness started), codegen:transpile and
codegen:transpile_with_request_inventory. The entrypoint's exit trap removes its work
dir, so a row sees that dir only through what the stubs copied while it existed.
Two more events come from fixtures: cp:fail (the stub cp refused the entrypoint's
snapshot copy) and race:link (a racing writer created the output the instant the
helper linked it). Scratch is pytest's tmp_path; this file calls no deletion.
"""
import ast
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
ENTRYPOINT = ROOT / "docker" / "entrypoint.sh"
HELPER = ROOT / "docker" / "bind_compiled_inventory.py"

SCHEMA = "pineforge-request-feed-inventory/v1"
POLICY = "selected-window/v1"
PINE_TEXT = "//@version=6\nstrategy('pipeline fixture')\n"
CPP_BYTES = "// precompiled é\nint pf_precompiled = 1;\n".encode("utf-8")
SO_BYTES = b"fake shared object\n"

SAME_TICKER_DAILY = [{"kind": "token", "timeframe": "D"}]
EMPTY = []
MIXED = [{"kind": "token", "timeframe": "D"},
         {"kind": "input", "key": "HTF", "default": "240"},
         {"kind": "unknown"}]
ENTRY_SETS = [SAME_TICKER_DAILY, EMPTY, MIXED]
ENTRY_IDS = ["same-ticker-daily", "empty", "mixed-input-and-unknown"]

SELECTED_ENV = {
    "PINEFORGE_REPORT_POLICY": POLICY,
    "PINEFORGE_WINDOW_START_MS": "60000",
    "PINEFORGE_WINDOW_END_MS": "240000",
    "PINEFORGE_PREROLL_BARS": "1",
    "PINEFORGE_FED_START_MS": "0",
}
SELECTED_FLAGS = ["--report-policy", POLICY, "--window-start-ms", "60000",
                  "--window-end-ms", "240000", "--preroll-bars", "1", "--fed-start-ms", "0"]
BINDING_FAILED = "request-feed inventory binding failed"

# --- The fakes and stubs the private tree is made of -----------------------------

FAKE_CODEGEN_INIT = r'''
import hashlib
import json
import os


def _event(name):
    with open(os.path.join(os.environ["STUB_LOG"], "events"), "a") as handle:
        handle.write(name + "\n")


def _cpp(source):
    return "// généré\nint pf_fake_strategy = %d;\n" % len(source)


def transpile(pine_source, *, check_support=True, filename="<input>", libraries=None):
    _event("codegen:transpile")
    return _cpp(pine_source)


def transpile_with_request_inventory(pine_source, **kwargs):
    _event("codegen:transpile_with_request_inventory")
    with open(os.path.join(os.environ["STUB_LOG"], "codegen.kwargs.json"), "w") as handle:
        json.dump(sorted(kwargs), handle)
    if os.environ.get("FAKE_CODEGEN_FAIL"):
        from .errors import CompileError
        raise CompileError("forced transpile failure")
    cpp = _cpp(pine_source)
    if os.environ.get("FAKE_CODEGEN_WRONG_SOURCE"):
        digest = hashlib.sha256(b"not the generated source").hexdigest()
    else:
        digest = hashlib.sha256(cpp.encode("utf-8")).hexdigest()
    return {
        "cpp": cpp,
        "request_feed_inventory": {
            "schema": "pineforge-request-feed-inventory/v1",
            "source_sha256": digest,
            "artifact_sha256": None,
            "primary_chart_timeframe": None,
            "entries": json.loads(os.environ.get("FAKE_CODEGEN_ENTRIES", "[]")),
        },
    }
'''

FAKE_CODEGEN_ERRORS = r'''
class CompileError(Exception):
    pass
'''

FAKE_CODEGEN_BINDER = r'''
import hashlib
import os


class RequestFeedInventoryError(ValueError):
    pass


def bind_request_feed_inventory(inventory, cpp_bytes, artifact_bytes):
    with open(os.path.join(os.environ["STUB_LOG"], "events"), "a") as handle:
        handle.write("pinned:bind\n")
    keys = ("schema", "source_sha256", "artifact_sha256", "primary_chart_timeframe", "entries")
    if not isinstance(inventory, dict) or set(inventory) != set(keys):
        raise RequestFeedInventoryError(
            "inventory is not an object with exactly the keys " + ", ".join(keys))
    if inventory["artifact_sha256"] is not None:
        raise RequestFeedInventoryError(
            "inventory artifact_sha256 is already set; only an unbound inventory can be bound")
    if hashlib.sha256(cpp_bytes).hexdigest() != inventory["source_sha256"]:
        raise RequestFeedInventoryError(
            "inventory source_sha256 does not match the supplied C++ bytes")
    return {
        "schema": inventory["schema"],
        "source_sha256": inventory["source_sha256"],
        "artifact_sha256": hashlib.sha256(artifact_bytes).hexdigest(),
        "primary_chart_timeframe": inventory["primary_chart_timeframe"],
        "entries": [dict(entry) for entry in inventory["entries"]],
    }
'''

GXX_STUB = r'''#!/usr/bin/env bash
out=""; src=""; prev=""
for a in "$@"; do
    [ "$prev" = "-o" ] && out="$a"
    case "$a" in *.cpp) src="$a" ;; esac
    prev="$a"
done
echo gxx >> "$STUB_LOG/events"
echo "$src" > "$STUB_LOG/gxx.source.path"
if [ -n "${STUB_GXX_MUTATE:-}" ]; then
    printf 'the caller changed this file after the snapshot\n' > "$STUB_GXX_MUTATE"
fi
if [ -n "${STUB_GXX_FAIL:-}" ]; then
    echo "stub g++: forced link failure" >&2
    exit 1
fi
/bin/cp "$src" "$STUB_LOG/gxx.source.bin"
printf 'fake shared object\n' > "$out"
/bin/cp "$out" "$STUB_LOG/gxx.artifact.bin"
'''

# Passes every cp through to the real one, except that with STUB_CP_FAIL set it refuses
# a copy whose destination is named strategy.cpp: the entrypoint's snapshot.
CP_STUB = r'''#!/usr/bin/env bash
if [ -n "${STUB_CP_FAIL:-}" ]; then
    for last in "$@"; do :; done
    case "$last" in
        */strategy.cpp)
            echo cp:fail >> "$STUB_LOG/events"
            echo "stub cp: forced copy failure" >&2
            exit 1
            ;;
    esac
fi
exec /bin/cp "$@"
'''

# Runs the real helper after replacing os.link with a link that first creates the output,
# as a racing writer would, at the very moment the helper publishes.
RACE_SHIM = r'''
import os
import runpy
import sys

racer = bytes.fromhex(os.environ["RACE_BYTES_HEX"])
real_link = os.link


def racing_link(src, dst, *args, **kwargs):
    with open(os.path.join(os.environ["STUB_LOG"], "events"), "a") as handle:
        handle.write("race:link\n")
    with open(dst, "wb") as handle:
        handle.write(racer)
    return real_link(src, dst, *args, **kwargs)


os.link = racing_link
real = os.environ["REAL_BIND_HELPER"]
sys.argv[0] = real
runpy.run_path(real, run_name="__main__")
'''

RUN_JSON_STUB = r'''
import json
import os
import shutil
import sys

log = os.environ["STUB_LOG"]
argv = sys.argv[1:]
with open(os.path.join(log, "events"), "a") as handle:
    handle.write("run_json\n")
with open(os.path.join(log, "run_json.argv.json"), "w") as handle:
    json.dump(argv, handle)
work = os.path.dirname(argv[argv.index("--so") + 1])
with open(os.path.join(log, "run_json.work.json"), "w") as handle:
    json.dump(sorted(os.listdir(work)), handle)
if "--request-feed-inventory" in argv:
    shutil.copyfile(argv[argv.index("--request-feed-inventory") + 1],
                    os.path.join(log, "run_json.inventory.json"))
if "--generated-cpp" in argv:
    shutil.copyfile(argv[argv.index("--generated-cpp") + 1],
                    os.path.join(log, "run_json.generated.bin"))
print('{"engine":"pineforge","marker":"run_json_stub"}')
'''

BINDER_SHIM = r'''
import json
import os
import runpy
import sys

log = os.environ["STUB_LOG"]
with open(os.path.join(log, "events"), "a") as handle:
    handle.write("helper\n")
with open(os.path.join(log, "helper.argv.json"), "w") as handle:
    json.dump(sys.argv[1:], handle)
real = os.environ["REAL_BIND_HELPER"]
sys.argv[0] = real
runpy.run_path(real, run_name="__main__")
'''


class Box:
    """One test's private tree: a fake install prefix with the stubs, a fake
    pineforge_codegen on PYTHONPATH (and an empty one that has no binder), a stub
    g++ first on PATH, an input dir, a TMPDIR the entrypoint's work dir is made in,
    a log dir the stubs write to and a work dir for the helper rows."""

    def __init__(self, root):
        self.root = root
        self.bin = root / "bin"
        self.prefix = root / "prefix"
        self.indir = root / "in"
        self.tmp = root / "tmp"
        self.log = root / "log"
        self.work = root / "work"
        self.fakepkgs = root / "fakepkgs"
        self.nopkgs = root / "nopkgs"
        package = self.fakepkgs / "pineforge_codegen"
        for directory in (self.bin, self.prefix / "bin", self.prefix / "lib",
                          self.prefix / "include", self.indir, self.tmp, self.log,
                          self.work, package, self.nopkgs / "pineforge_codegen"):
            directory.mkdir(parents=True)
        (package / "__init__.py").write_text(FAKE_CODEGEN_INIT, encoding="utf-8")
        (package / "errors.py").write_text(FAKE_CODEGEN_ERRORS, encoding="utf-8")
        (package / "request_feed_inventory.py").write_text(FAKE_CODEGEN_BINDER,
                                                           encoding="utf-8")
        (self.nopkgs / "pineforge_codegen" / "__init__.py").write_text("", encoding="utf-8")
        gxx = self.bin / "g++"
        gxx.write_text(GXX_STUB, encoding="utf-8")
        gxx.chmod(0o755)
        cp = self.bin / "cp"
        cp.write_text(CP_STUB, encoding="utf-8")
        cp.chmod(0o755)
        (self.prefix / "bin" / "run_json.py").write_text(RUN_JSON_STUB, encoding="utf-8")
        (self.prefix / "bin" / "bind_compiled_inventory.py").write_text(BINDER_SHIM,
                                                                        encoding="utf-8")
        (self.prefix / "lib" / "libpineforge.a").write_bytes(b"")
        (self.indir / "ohlcv.csv").write_text("timestamp,open,high,low,close,volume\n",
                                              encoding="utf-8")

    def env(self, extra=None, pythonpath=None):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("PINEFORGE_", "STUB_", "FAKE_", "REAL_"))}
        search = str(self.fakepkgs if pythonpath is None else pythonpath)
        if os.environ.get("PYTHONPATH"):
            search += os.pathsep + os.environ["PYTHONPATH"]
        env.update({
            "PATH": str(self.bin) + os.pathsep + os.environ.get("PATH", ""),
            "PYTHONPATH": search,
            "PYTHONUTF8": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "TMPDIR": str(self.tmp),
            "STUB_LOG": str(self.log),
            "REAL_BIND_HELPER": str(HELPER),
            "PINEFORGE_PREFIX": str(self.prefix),
            "PINEFORGE_IN_DIR": str(self.indir),
        })
        env.update(extra or {})
        return env

    def events(self):
        path = self.log / "events"
        return path.read_text(encoding="utf-8").splitlines() if path.exists() else []

    def logged(self, name):
        return json.loads((self.log / name).read_text(encoding="utf-8"))


@pytest.fixture
def box(tmp_path):
    return Box(tmp_path)


# --- Fixtures and drivers --------------------------------------------------------

def sha(data):
    return hashlib.sha256(data).hexdigest()


def fake_cpp(pine_text):
    return "// généré\nint pf_fake_strategy = %d;\n" % len(pine_text)


def source_inventory(cpp_bytes, entries, artifact=None, chart=None):
    return {"schema": SCHEMA, "source_sha256": sha(cpp_bytes), "artifact_sha256": artifact,
            "primary_chart_timeframe": chart, "entries": entries}


def bound_inventory(cpp_bytes, so_bytes, entries):
    return source_inventory(cpp_bytes, entries, artifact=sha(so_bytes))


def old_argv(transpiled):
    """run_json.py's argv on the unchanged path, with the three paths masked."""
    return ["--so", "<path>", "--ohlcv", "<path>", "--inputs", "", "--overrides", "",
            "--input-tf", "", "--script-tf", "", "--bar-magnifier", "",
            "--magnifier-samples", "4", "--magnifier-dist", "endpoints",
            "--generated-cpp", "<path>", "--transpiled", transpiled]


def normalized(argv):
    masked = list(argv)
    for flag in ("--so", "--ohlcv", "--generated-cpp", "--request-feed-inventory"):
        if flag in masked:
            masked[masked.index(flag) + 1] = "<path>"
    return masked


def put_pine(box):
    (box.indir / "strategy.pine").write_text(PINE_TEXT, encoding="utf-8")


def put_cpp(box):
    (box.indir / "strategy.cpp").write_bytes(CPP_BYTES)


def put_supplied(box, inventory):
    path = box.root / "supplied-inventory.json"
    path.write_text(json.dumps(inventory), encoding="utf-8")
    return path


def run_entrypoint(box, extra=None, xtrace=False, pythonpath=None):
    command = ["bash", "-x", str(ENTRYPOINT)] if xtrace else ["bash", str(ENTRYPOINT)]
    return subprocess.run(command, env=box.env(extra, pythonpath), stdin=subprocess.DEVNULL,
                          capture_output=True, encoding="utf-8", errors="replace",
                          timeout=120, check=False)


def run_helper(box, inventory, cpp, so, out, pythonpath=None):
    command = [sys.executable, str(HELPER), "--inventory", str(inventory), "--cpp", str(cpp),
               "--so", str(so), "--out", str(out)]
    return subprocess.run(command, env=box.env(None, pythonpath), stdin=subprocess.DEVNULL,
                          capture_output=True, encoding="utf-8", errors="replace",
                          timeout=120, check=False)


def helper_files(box, inventory=None, entries=SAME_TICKER_DAILY):
    cpp, so = box.work / "strategy.cpp", box.work / "strategy.so"
    supplied = box.work / "supplied.json"
    cpp.write_bytes(CPP_BYTES)
    so.write_bytes(SO_BYTES)
    document = source_inventory(CPP_BYTES, entries) if inventory is None else inventory
    supplied.write_text(json.dumps(document), encoding="utf-8")
    return supplied, cpp, so, box.work / "bound.json"


# --- The helper: bind_compiled_inventory.py --------------------------------------

@pytest.mark.parametrize("entries", ENTRY_SETS, ids=ENTRY_IDS)
def test_helper_binds_without_dropping_or_reducing_entries(box, entries):
    supplied, cpp, so, out = helper_files(box, entries=entries)
    before = supplied.read_bytes()
    result = run_helper(box, supplied, cpp, so, out)
    assert result.returncode == 0, result.stderr
    bound = json.loads(out.read_text(encoding="utf-8"))
    assert bound == bound_inventory(CPP_BYTES, SO_BYTES, entries)
    assert bound["primary_chart_timeframe"] is None
    assert supplied.read_bytes() == before
    assert box.events() == ["pinned:bind"]


def test_helper_refuses_a_wrong_source_digest(box):
    other = source_inventory(b"some other generated source", SAME_TICKER_DAILY)
    supplied, cpp, so, out = helper_files(box, inventory=other)
    before = supplied.read_bytes()
    result = run_helper(box, supplied, cpp, so, out)
    assert result.returncode == 14
    assert result.stderr.startswith("bind_compiled_inventory: ")
    assert "source_sha256" in result.stderr
    assert not out.exists()
    assert supplied.read_bytes() == before


def test_helper_refuses_an_inventory_that_is_already_bound(box):
    bound = bound_inventory(CPP_BYTES, SO_BYTES, SAME_TICKER_DAILY)
    supplied, cpp, so, out = helper_files(box, inventory=bound)
    result = run_helper(box, supplied, cpp, so, out)
    assert result.returncode == 14
    assert "already set" in result.stderr
    assert not out.exists()


BAD_INVENTORY_FILES = [
    (b"", 11),
    (b'{"schema":', 11),
    (b'{"schema": "\xff"}', 11),
    (b"\xef\xbb\xbf{}", 11),
    (b'{"schema": "a", "schema": "b"}', 11),
    (b'{"primary_chart_timeframe": NaN}', 11),
    (b"{} trailing", 11),
    (b"[]", 14),
]
BAD_INVENTORY_IDS = ["empty", "truncated", "bad-utf8", "bom", "duplicate-key", "nan",
                     "trailing-garbage", "not-an-object"]


@pytest.mark.parametrize("data,code", BAD_INVENTORY_FILES, ids=BAD_INVENTORY_IDS)
def test_helper_refuses_a_malformed_inventory(box, data, code):
    supplied, cpp, so, out = helper_files(box)
    supplied.write_bytes(data)
    result = run_helper(box, supplied, cpp, so, out)
    assert result.returncode == code, result.stderr
    assert result.stderr.startswith("bind_compiled_inventory: ")
    assert not out.exists()
    assert supplied.read_bytes() == data


def test_helper_refuses_an_oversize_inventory(box):
    supplied, cpp, so, out = helper_files(box)
    supplied.write_bytes(b" " * (8 * 1024 * 1024 + 1))
    result = run_helper(box, supplied, cpp, so, out)
    assert result.returncode == 10
    assert "over" in result.stderr
    assert not out.exists()


def test_helper_refuses_an_unreadable_inventory(box):
    supplied, cpp, so, out = helper_files(box)
    result = run_helper(box, box.work / "absent.json", cpp, so, out)
    assert result.returncode == 10
    assert "cannot be read" in result.stderr
    assert not out.exists()


@pytest.mark.parametrize("missing", ["cpp", "so"])
def test_helper_refuses_an_unreadable_source_or_library(box, missing):
    supplied, cpp, so, out = helper_files(box)
    absent = box.work / "absent.bin"
    result = run_helper(box, supplied, absent if missing == "cpp" else cpp,
                        absent if missing == "so" else so, out)
    assert result.returncode == 12
    assert "cannot be read" in result.stderr
    assert not out.exists()
    assert box.events() == []


def test_helper_never_replaces_a_file(box):
    supplied, cpp, so, out = helper_files(box)
    before = supplied.read_bytes()
    same = run_helper(box, supplied, cpp, so, supplied)
    assert same.returncode == 15
    assert "exists already" in same.stderr
    assert supplied.read_bytes() == before
    out.write_bytes(b"earlier output")
    again = run_helper(box, supplied, cpp, so, out)
    assert again.returncode == 15
    assert out.read_bytes() == b"earlier output"
    assert box.events() == []


def test_helper_refuses_an_output_directory_that_does_not_exist(box):
    supplied, cpp, so, _ = helper_files(box)
    result = run_helper(box, supplied, cpp, so, box.work / "absent-dir" / "bound.json")
    assert result.returncode == 15
    assert "does not exist" in result.stderr


def test_helper_publishes_by_link_so_the_temporary_name_is_a_second_name(box):
    supplied, cpp, so, out = helper_files(box)
    result = run_helper(box, supplied, cpp, so, out)
    assert result.returncode == 0, result.stderr
    names = sorted(path.name for path in box.work.iterdir())
    temporaries = [name for name in names if name.startswith(".bound-inventory-")]
    assert len(temporaries) == 1
    assert names == sorted(["bound.json", "strategy.cpp", "strategy.so", "supplied.json"]
                           + temporaries)
    temporary = box.work / temporaries[0]
    assert os.path.samefile(temporary, out)
    assert out.stat().st_nlink == 2
    assert temporary.read_bytes() == out.read_bytes()
    assert out.read_text(encoding="utf-8").endswith("\n")


def test_helper_refuses_and_keeps_the_original_bytes_when_out_appears_at_the_link(box):
    supplied, cpp, so, out = helper_files(box)
    before = supplied.read_bytes()
    racer = b"the racing writer's original bytes\n"
    shim = box.root / "race_helper.py"
    shim.write_text(RACE_SHIM, encoding="utf-8")
    command = [sys.executable, str(shim), "--inventory", str(supplied), "--cpp", str(cpp),
               "--so", str(so), "--out", str(out)]
    result = subprocess.run(command, env=box.env({"RACE_BYTES_HEX": racer.hex()}),
                            stdin=subprocess.DEVNULL, capture_output=True, encoding="utf-8",
                            errors="replace", timeout=120, check=False)
    assert result.returncode == 15, result.stderr
    assert result.stderr.startswith("bind_compiled_inventory: ")
    assert "exists already" in result.stderr
    assert box.events() == ["pinned:bind", "race:link"]
    assert out.read_bytes() == racer
    assert out.stat().st_nlink == 1
    assert supplied.read_bytes() == before
    leftovers = [path for path in box.work.iterdir() if path.name.startswith(".bound-inventory-")]
    assert len(leftovers) == 1
    assert not os.path.samefile(leftovers[0], out)


def test_helper_refuses_when_the_pinned_binder_is_missing(box):
    supplied, cpp, so, out = helper_files(box)
    result = run_helper(box, supplied, cpp, so, out, pythonpath=box.nopkgs)
    assert result.returncode == 13
    assert "bind_request_feed_inventory" in result.stderr
    assert not out.exists()


def test_helper_source_reimplements_nothing_and_loads_no_library():
    tree = ast.parse(HELPER.read_text(encoding="utf-8"))
    imported = set()
    pinned_names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            imported.add((node.module or "").split(".")[0])
            if node.module == "pineforge_codegen.request_feed_inventory":
                pinned_names.update(alias.name for alias in node.names)
    assert not imported & {"ctypes", "cffi", "subprocess", "hashlib", "re", "importlib",
                           "runpy", "socket", "urllib", "http", "shutil"}
    assert "bind_request_feed_inventory" in pinned_names
    called = {node.func.attr for node in ast.walk(tree)
              if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)}
    assert not called & {"sha256", "sha1", "md5", "CDLL", "LoadLibrary", "dlopen", "system",
                         "popen", "unlink", "remove", "rmdir", "rmtree", "removedirs",
                         "rename", "renames", "replace"}
    assert "link" in called


# --- The entrypoint ---------------------------------------------------------------

def test_entrypoint_adds_no_deletion_beyond_its_one_exit_trap():
    text = ENTRYPOINT.read_text(encoding="utf-8")
    assert re.findall(r"\brm\b", text) == ["rm"]
    assert "trap 'rm -rf \"$WORK\"' EXIT" in text


@pytest.mark.parametrize("kind", ["pine", "cpp"])
def test_off_branch_reads_no_inventory_and_keeps_the_old_argv(box, kind):
    sentinel = "/nonexistent/off-branch-sentinel-inventory.json"
    put_pine(box) if kind == "pine" else put_cpp(box)
    extra = dict(SELECTED_ENV, PINEFORGE_REQUEST_FEED_INVENTORY=sentinel, STUB_CP_FAIL="1")
    del extra["PINEFORGE_REPORT_POLICY"]
    result = run_entrypoint(box, extra, xtrace=True)
    assert result.returncode == 0, result.stderr
    assert "run_json_stub" in result.stdout
    assert box.events() == (["codegen:transpile", "gxx", "run_json"] if kind == "pine"
                            else ["gxx", "run_json"])
    argv = box.logged("run_json.argv.json")
    assert normalized(argv) == old_argv("true" if kind == "pine" else "false")
    assert not any("inventory" in name for name in box.logged("run_json.work.json"))
    if kind == "cpp":
        # The ordinary path still compiles and reports the caller's own file, with no
        # snapshot (a stub cp that refuses the snapshot is on PATH and never reached).
        caller = str(box.indir / "strategy.cpp")
        assert argv[argv.index("--generated-cpp") + 1] == caller
        assert (box.log / "gxx.source.path").read_text(encoding="utf-8").strip() == caller
        assert box.logged("run_json.work.json") == ["strategy.so"]
    assert sentinel not in result.stderr
    assert not (box.log / "helper.argv.json").exists()


def test_transpile_only_keeps_the_old_output_and_reads_no_inventory(box):
    sentinel = "/nonexistent/transpile-only-sentinel-inventory.json"
    put_pine(box)
    result = run_entrypoint(box, {"PINEFORGE_TRANSPILE_ONLY": "1",
                                  "PINEFORGE_REPORT_POLICY": POLICY,
                                  "PINEFORGE_REQUEST_FEED_INVENTORY": sentinel}, xtrace=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout == fake_cpp(PINE_TEXT)
    assert box.events() == ["codegen:transpile"]
    assert sentinel not in result.stderr


@pytest.mark.parametrize("entries", ENTRY_SETS, ids=ENTRY_IDS)
def test_selected_pine_path_keeps_the_source_and_binds_after_linking(box, entries):
    put_pine(box)
    result = run_entrypoint(box, dict(SELECTED_ENV, FAKE_CODEGEN_ENTRIES=json.dumps(entries)))
    assert result.returncode == 0, result.stderr
    assert box.events() == ["codegen:transpile_with_request_inventory", "gxx", "helper",
                            "pinned:bind", "run_json"]
    assert box.logged("codegen.kwargs.json") == ["filename"]
    cpp = fake_cpp(PINE_TEXT).encode("utf-8")
    assert (box.log / "gxx.source.bin").read_bytes() == cpp
    artifact = (box.log / "gxx.artifact.bin").read_bytes()
    assert normalized(box.logged("run_json.argv.json")) == (
        old_argv("true") + SELECTED_FLAGS + ["--request-feed-inventory", "<path>"])
    assert box.logged("run_json.inventory.json") == bound_inventory(cpp, artifact, entries)
    assert {"strategy.cpp", "strategy.so", "request_feed_inventory.source.json",
            "request_feed_inventory.json"} <= set(box.logged("run_json.work.json"))


@pytest.mark.parametrize("entries", ENTRY_SETS, ids=ENTRY_IDS)
def test_selected_precompiled_cpp_binds_the_callers_inventory(box, entries):
    put_cpp(box)
    supplied = put_supplied(box, source_inventory(CPP_BYTES, entries))
    before = supplied.read_bytes()
    result = run_entrypoint(box, dict(SELECTED_ENV,
                                      PINEFORGE_REQUEST_FEED_INVENTORY=str(supplied)))
    assert result.returncode == 0, result.stderr
    assert box.events() == ["gxx", "helper", "pinned:bind", "run_json"]
    assert (box.log / "gxx.source.bin").read_bytes() == CPP_BYTES
    artifact = (box.log / "gxx.artifact.bin").read_bytes()
    assert box.logged("run_json.inventory.json") == bound_inventory(CPP_BYTES, artifact, entries)
    assert supplied.read_bytes() == before
    helper_argv = box.logged("helper.argv.json")
    assert helper_argv[helper_argv.index("--inventory") + 1] == str(supplied)
    run_argv = box.logged("run_json.argv.json")
    assert run_argv[run_argv.index("--request-feed-inventory") + 1] != str(supplied)


@pytest.mark.parametrize("supplied_env", [{}, {"PINEFORGE_REQUEST_FEED_INVENTORY": ""}],
                         ids=["unset", "empty"])
def test_selected_precompiled_cpp_without_inventory_gets_none_never_an_empty_one(box,
                                                                                 supplied_env):
    put_cpp(box)
    result = run_entrypoint(box, dict(SELECTED_ENV, **supplied_env))
    assert result.returncode == 0, result.stderr
    assert box.events() == ["gxx", "run_json"]
    argv = box.logged("run_json.argv.json")
    assert "--request-feed-inventory" not in argv
    assert normalized(argv) == old_argv("false") + SELECTED_FLAGS
    assert not any("inventory" in name for name in box.logged("run_json.work.json"))


def test_a_supplied_inventory_is_not_used_with_a_pine_input(box):
    sentinel = "/nonexistent/pine-input-sentinel-inventory.json"
    put_pine(box)
    result = run_entrypoint(box, dict(SELECTED_ENV, PINEFORGE_REQUEST_FEED_INVENTORY=sentinel),
                            xtrace=True)
    assert result.returncode == 0, result.stderr
    assert box.events() == ["codegen:transpile_with_request_inventory", "gxx", "helper",
                            "pinned:bind", "run_json"]
    helper_argv = box.logged("helper.argv.json")
    assert helper_argv[helper_argv.index("--inventory") + 1].endswith(
        "request_feed_inventory.source.json")
    assert sentinel not in result.stderr


@pytest.mark.parametrize("kind", ["pine", "cpp"])
def test_link_failure_stops_before_the_binder(box, kind):
    if kind == "pine":
        put_pine(box)
        extra = dict(SELECTED_ENV, STUB_GXX_FAIL="1")
    else:
        put_cpp(box)
        supplied = put_supplied(box, source_inventory(CPP_BYTES, SAME_TICKER_DAILY))
        extra = dict(SELECTED_ENV, STUB_GXX_FAIL="1",
                     PINEFORGE_REQUEST_FEED_INVENTORY=str(supplied))
    result = run_entrypoint(box, extra)
    assert result.returncode == 3
    assert "compile failed" in result.stderr
    assert box.events()[-1] == "gxx"
    assert not (box.log / "helper.argv.json").exists()
    assert not (box.log / "run_json.argv.json").exists()
    assert result.stdout == ""


def test_binder_refusal_stops_before_run_json_with_no_fallback(box):
    put_cpp(box)
    supplied = put_supplied(box, source_inventory(b"some other generated source",
                                                  SAME_TICKER_DAILY))
    result = run_entrypoint(box, dict(SELECTED_ENV,
                                      PINEFORGE_REQUEST_FEED_INVENTORY=str(supplied)))
    assert result.returncode == 3
    assert box.events() == ["gxx", "helper", "pinned:bind"]
    assert BINDING_FAILED in result.stderr
    assert "source_sha256" in result.stderr
    assert result.stdout == ""
    assert not (box.log / "run_json.argv.json").exists()


def test_a_pine_inventory_whose_source_digest_is_wrong_stops_before_run_json(box):
    put_pine(box)
    result = run_entrypoint(box, dict(SELECTED_ENV, FAKE_CODEGEN_WRONG_SOURCE="1"))
    assert result.returncode == 3
    assert box.events() == ["codegen:transpile_with_request_inventory", "gxx", "helper",
                            "pinned:bind"]
    assert BINDING_FAILED in result.stderr
    assert not (box.log / "run_json.argv.json").exists()


@pytest.mark.parametrize("content", [None, b"", b"{", b'{"schema": 1, "schema": 2}'],
                         ids=["absent", "empty", "truncated", "duplicate-key"])
def test_an_unreadable_or_malformed_supplied_inventory_ends_the_run(box, content):
    put_cpp(box)
    supplied = box.root / "supplied-inventory.json"
    if content is not None:
        supplied.write_bytes(content)
    result = run_entrypoint(box, dict(SELECTED_ENV,
                                      PINEFORGE_REQUEST_FEED_INVENTORY=str(supplied)))
    assert result.returncode == 3
    assert box.events() == ["gxx", "helper"]
    assert BINDING_FAILED in result.stderr
    assert not (box.log / "run_json.argv.json").exists()


@pytest.mark.parametrize("kind", ["pine", "cpp"])
def test_an_unknown_policy_is_forwarded_raw_and_binds_nothing(box, kind):
    put_pine(box) if kind == "pine" else put_cpp(box)
    result = run_entrypoint(box, {"PINEFORGE_REPORT_POLICY": "selected-window/v2",
                                  "PINEFORGE_WINDOW_START_MS": "1",
                                  "PINEFORGE_REQUEST_FEED_INVENTORY": "/nonexistent/x.json",
                                  "STUB_CP_FAIL": "1"})
    assert result.returncode == 0, result.stderr
    assert box.events() == (["codegen:transpile", "gxx", "run_json"] if kind == "pine"
                            else ["gxx", "run_json"])
    argv = box.logged("run_json.argv.json")
    assert normalized(argv) == old_argv("true" if kind == "pine" else "false") + [
        "--report-policy", "selected-window/v2", "--window-start-ms", "1"]
    if kind == "cpp":
        assert argv[argv.index("--generated-cpp") + 1] == str(box.indir / "strategy.cpp")


def test_selected_precompiled_cpp_is_frozen_once_and_every_consumer_uses_the_copy(box):
    put_cpp(box)
    caller = box.indir / "strategy.cpp"
    supplied = put_supplied(box, source_inventory(CPP_BYTES, SAME_TICKER_DAILY))
    result = run_entrypoint(box, dict(SELECTED_ENV,
                                      PINEFORGE_REQUEST_FEED_INVENTORY=str(supplied),
                                      STUB_GXX_MUTATE=str(caller)))
    assert result.returncode == 0, result.stderr
    assert box.events() == ["gxx", "helper", "pinned:bind", "run_json"]
    # Control: the caller's file really was changed after the snapshot was taken.
    assert caller.read_bytes() != CPP_BYTES
    # The compiler was given a private file under the entrypoint's work dir and saw the
    # original bytes, not the changed ones.
    compiled = (box.log / "gxx.source.path").read_text(encoding="utf-8").strip()
    assert compiled != str(caller)
    assert compiled.startswith(str(box.tmp) + os.sep)
    assert os.path.basename(compiled) == "strategy.cpp"
    assert (box.log / "gxx.source.bin").read_bytes() == CPP_BYTES
    # The binder was handed that same file and bound the original bytes ...
    helper_argv = box.logged("helper.argv.json")
    assert helper_argv[helper_argv.index("--cpp") + 1] == compiled
    artifact = (box.log / "gxx.artifact.bin").read_bytes()
    assert box.logged("run_json.inventory.json") == bound_inventory(CPP_BYTES, artifact,
                                                                   SAME_TICKER_DAILY)
    # ... and run_json's provenance file is the same private file, still not transpiled.
    run_argv = box.logged("run_json.argv.json")
    assert run_argv[run_argv.index("--generated-cpp") + 1] == compiled
    assert run_argv[run_argv.index("--transpiled") + 1] == "false"
    assert (box.log / "run_json.generated.bin").read_bytes() == CPP_BYTES


def test_a_failed_snapshot_ends_the_selected_path_before_compile_and_run(box):
    put_cpp(box)
    supplied = put_supplied(box, source_inventory(CPP_BYTES, SAME_TICKER_DAILY))
    result = run_entrypoint(box, dict(SELECTED_ENV,
                                      PINEFORGE_REQUEST_FEED_INVENTORY=str(supplied),
                                      STUB_CP_FAIL="1"))
    assert result.returncode == 3
    assert "cannot snapshot strategy.cpp into the work dir" in result.stderr
    assert box.events() == ["cp:fail"]
    assert result.stdout == ""


def test_a_source_digest_claimed_by_the_strategy_code_is_not_accepted(box):
    built_for = b"the bytes the inventory was built for\n"
    claiming = ("// source_sha256: " + sha(built_for) + "\n").encode("ascii") + CPP_BYTES
    (box.indir / "strategy.cpp").write_bytes(claiming)
    supplied = put_supplied(box, source_inventory(built_for, SAME_TICKER_DAILY))
    result = run_entrypoint(box, dict(SELECTED_ENV,
                                      PINEFORGE_REQUEST_FEED_INVENTORY=str(supplied)))
    assert result.returncode == 3
    assert box.events() == ["gxx", "helper", "pinned:bind"]
    assert BINDING_FAILED in result.stderr
    assert "source_sha256" in result.stderr
    assert not (box.log / "run_json.argv.json").exists()


def test_window_flags_keep_raw_values_and_invent_no_default(box):
    put_cpp(box)
    result = run_entrypoint(box, {"PINEFORGE_REPORT_POLICY": POLICY,
                                  "PINEFORGE_WINDOW_START_MS": " 12 ",
                                  "PINEFORGE_WINDOW_END_MS": "abc"})
    assert result.returncode == 0, result.stderr
    assert normalized(box.logged("run_json.argv.json")) == old_argv("false") + [
        "--report-policy", POLICY, "--window-start-ms", " 12 ", "--window-end-ms", "abc"]


def test_selected_transpile_failure_is_exit_5_with_no_ordinary_fallback(box):
    put_pine(box)
    result = run_entrypoint(box, dict(SELECTED_ENV, FAKE_CODEGEN_FAIL="1"))
    assert result.returncode == 5
    assert "transpile error" in result.stderr
    assert box.events() == ["codegen:transpile_with_request_inventory"]


def test_a_codegen_without_the_inventory_api_is_exit_5_with_no_ordinary_fallback(box):
    put_pine(box)
    result = run_entrypoint(box, SELECTED_ENV, pythonpath=box.nopkgs)
    assert result.returncode == 5
    assert "no request-feed inventory API" in result.stderr
    assert box.events() == []
