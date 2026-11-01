"""Outside-in compile-stage tests; run only on the Linux proof host."""

import errno
import fcntl
import json
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import stat
import subprocess
import time

import pytest


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
# Pinned to main at the merge; when main moves again before this change merges, repoint all three to the new main head (and re-run the re-anchor and the OFF row).
BASE = "69cddc9d102d2ccf5605231cc508b82c08c7081c"
ENTRYPOINT = Path(os.environ.get("PINEFORGE_ENTRYPOINT_UNDER_TEST",
                                str(ROOT / "docker/entrypoint.sh"))).resolve()
WALL = "PINEFORGE_COMPILE_WALL_MS"
ADDRESS = "PINEFORGE_COMPILE_AS_BYTES"
DIAGNOSTIC = "PINEFORGE_COMPILE_DIAGNOSTIC_MAX_BYTES"
MAXIMUM = (1 << 53) - 2
COMPILE_LOG = b"[pineforge] compiling strategy.cpp ...\n"
BIND_LOG = b"[pineforge] binding request-feed inventory ...\n"
COMPILE_FAILURE = b"[pineforge] compile failed\n"
PROBE_FAILURE = b"fixture compiler: requested probe failure\n"
BINDER_FAILURE = b"fixture binder: requested inventory failure\n"


class Invocation:
    def __init__(self, result, elapsed, trace):
        self.returncode = result.returncode
        self.stdout = result.stdout
        self.stderr = result.stderr
        self.elapsed = elapsed
        self.trace = trace
        self.records = {path.stem: json.loads(path.read_bytes())
                        for path in trace.glob("*.json")}
        self.argv = (trace / "argv.bin").read_bytes() if (trace / "argv.bin").exists() else None
        self.environment = ((trace / "environment.bin").read_bytes()
                            if (trace / "environment.bin").exists() else None)
        self.syscalls = ((trace / "syscalls.log").read_text()
                         if (trace / "syscalls.log").exists() else "")
        self.artifact_sha256 = ((trace / "artifact.sha256").read_text()
                                if (trace / "artifact.sha256").exists() else None)


class Box:
    def __init__(self, root, reference):
        self.root = root
        self.reference = reference
        self.indir = root / "in"
        self.prefix = root / "prefix"
        self.tools = root / "tools"
        self.trace = root / "trace"
        self.processes = []
        for directory in (self.indir, self.prefix / "bin", self.prefix / "lib",
                          self.tools, self.trace):
            directory.mkdir(parents=True)
        archive = Path(os.environ.get("PINEFORGE_ENTRYPOINT_TEST_ARCHIVE",
                                      str(ROOT / "build/lib/libpineforge.a"))).resolve()
        assert archive.is_file(), f"build the complete Release profile first: {archive}"
        (self.prefix / "lib/libpineforge.a").symlink_to(archive)
        generated_version = archive.parent.parent / "include/pineforge/version.h"
        assert generated_version.is_file(), (
            f"the archive's generated version header is required: {generated_version}")
        source_include = ROOT / "include"
        include = self.prefix / "include"
        include.mkdir()
        for source in sorted(source_include.rglob("*")):
            destination = include / source.relative_to(source_include)
            if source.is_dir():
                destination.mkdir()
            else:
                destination.symlink_to(source)
        (include / "pineforge/version.h").symlink_to(generated_version)
        for destination, source in (
                (self.tools / "g++", "compiler.py"),
                (self.tools / "mktemp", "mktemp.py"),
                (self.prefix / "bin/run_json.py", "runner.py"),
                (self.prefix / "bin/bind_compiled_inventory.py", "binder.py")):
            destination.write_bytes((FIXTURES / source).read_bytes())
            destination.chmod(destination.stat().st_mode | stat.S_IXUSR)
        (self.indir / "strategy.cpp").write_bytes((FIXTURES / "strategy.cpp").read_bytes())
        (self.indir / "ohlcv.csv").write_text(
            "timestamp,open,high,low,close,volume\n"
            "1700000000000,10,13,9,12,1\n"
            "1700000060000,12,13,8,9,2\n", encoding="utf-8")
        self.real_gxx = shutil.which("g++")
        assert self.real_gxx, "the proof requires the real g++ toolchain"
        self.env = {name: value for name, value in os.environ.items()
                    if not name.startswith(("PINEFORGE_", "PF_ENTRYPOINT_FIXTURE_"))}
        self.env.update({
            "PATH": f"{self.tools}:{os.environ['PATH']}",
            "PINEFORGE_PREFIX": str(self.prefix),
            "PINEFORGE_IN_DIR": str(self.indir),
            "PINEFORGE_ALLOW_UNATTESTED_CPP": "1",
            "TMPDIR": str(root),
            "PF_ENTRYPOINT_FIXTURE_GXX": self.real_gxx,
            "PF_ENTRYPOINT_FIXTURE_TRACE": str(self.trace),
            "PF_ENTRYPOINT_FIXTURE_WORK": str(root / "work"),
            "PF_ENTRYPOINT_FIXTURE_RUN_STARTED": str(root / "run-started"),
        })

    def run(self, extra=None, *, entrypoint=ENTRYPOINT, trace_reads=False,
            trace_processes=False, inherited_as=None, stderr_sink=None):
        env = dict(self.env)
        env.update(extra or {})
        command = ["bash", str(entrypoint)]
        if trace_reads or trace_processes:
            strace = shutil.which("strace")
            assert strace, "strace is required for the outside syscall observations"
            population = ",".join(name for name, enabled in (
                ("read", trace_reads), ("process", trace_processes)) if enabled)
            command = [strace, "-f", "-qq", "-yy", "-s", "0", "-e", f"trace={population}",
                       "-o", str(self.trace / "syscalls.log"), *command]
        def inherit_limits():
            if inherited_as is not None:
                resource.setrlimit(resource.RLIMIT_AS, (inherited_as, inherited_as))

        started = time.monotonic()
        process = subprocess.Popen(command, env=env, cwd=self.root, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE if stderr_sink is None else stderr_sink,
                                   start_new_session=True,
                                   preexec_fn=inherit_limits if inherited_as is not None else None)
        self.processes.append(process)
        # No outside time limit: the run ends when the entrypoint does (a real hang is caught by
        # the platform's own maximum job time (the workflows set no timeout-minutes: ci.yml:25)).
        stdout, stderr = process.communicate(input=b"")
        if stderr_sink is not None:
            stderr = Path(stderr_sink.name).read_bytes()
        result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
        return Invocation(result, time.monotonic() - started, self.trace)

    def stop_fixture_processes(self, process):
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for path in self.trace.glob("*.json"):
            document = json.loads(path.read_bytes())
            if "pid" not in document:
                continue
            pid = document["pid"]
            try:
                fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
                if "start_ticks" in document and fields[19] != document["start_ticks"]:
                    continue
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except FileNotFoundError:
                pass

    def payload(self, data, name="diagnostic"):
        path = self.root / name
        path.write_bytes(data)
        return str(path)


@pytest.fixture(scope="session")
def reference(tmp_path_factory):
    path = tmp_path_factory.mktemp("entrypoint-reference") / "entrypoint.sh"
    result = subprocess.run(["git", "show", f"{BASE}:docker/entrypoint.sh"],
                            cwd=ROOT, capture_output=True, check=True)
    path.write_bytes(result.stdout)
    return path


@pytest.fixture
def box(tmp_path, reference):
    instance = Box(tmp_path, reference)
    try:
        yield instance
    finally:
        # Cleanup follows all observations/assertions, including on a red run.
        for process in instance.processes:
            instance.stop_fixture_processes(process)


def assert_failure(result, code, args, status=4):
    assert result.returncode == status, (result.returncode, result.stdout, result.stderr)
    lines = result.stdout.splitlines()
    assert len(lines) == 1, result.stdout
    failure = json.loads(lines[0])
    assert failure["engine"] == "pineforge"
    assert failure["code"] == code
    assert failure["args"] == args
    assert isinstance(failure["error"], str) and failure["error"]
    assert not (result.trace.parent / "run-started").exists()


def assert_tree_gone(result):
    for role in ("driver", "middle", "helper"):
        record = result.records[role]
        assert not original_process_present(record), (role, record)
    heartbeat = result.trace / "heartbeat"
    before = heartbeat.read_bytes()
    time.sleep(0.1)
    assert heartbeat.read_bytes() == before, "compile helper survived the entrypoint"


def original_process_present(record):
    try:
        fields = Path(f"/proc/{record['pid']}/stat").read_text().rsplit(")", 1)[1].split()
    except (FileNotFoundError, ProcessLookupError):
        return False
    return fields[19] == record["start_ticks"]


def pipe_read_bytes(result, role="driver"):
    endpoint = result.records[role]["stdout"]
    assert endpoint.startswith("pipe:["), endpoint
    started = re.compile(r"^(?:\[pid\s+)?(\d+)\]?\s+read\(\d+<" + re.escape(endpoint) + r">,")
    finished = re.compile(r"\)\s+=\s+(\d+)")
    counts, split = [], set()
    for line in result.syscalls.splitlines():
        begin = started.search(line)
        if begin and "<unfinished ...>" in line:
            # strace -f prints a read that overlaps another process's syscall in two
            # parts: the start here, the byte count on a later "resumed" line.
            split.add(begin.group(1))
        elif begin and (match := finished.search(line)):
            counts.append(int(match.group(1)))
        elif "<... read resumed>" in line:
            pid = line.split(None, 1)[0]
            if pid in split and (match := finished.search(line)):
                split.discard(pid)
                counts.append(int(match.group(1)))
    assert counts, "the syscall observer saw no reads from the compiler diagnostic pipe"
    return sum(counts)


def test_off_argv_environment_descriptors_and_success_bytes_match_frozen_main(box):
    before = box.run(entrypoint=box.reference, trace_processes=True)
    after = box.run(trace_processes=True)
    assert before.returncode == after.returncode == 0, (before.stderr, after.stderr)
    assert before.stdout == after.stdout == b'{"signals":[2.0,-3.0]}\n'
    assert before.artifact_sha256 is not None
    assert before.artifact_sha256 == after.artifact_sha256
    assert before.stderr == after.stderr
    assert before.argv == after.argv
    assert before.environment == after.environment
    assert before.records["driver"]["as"] == after.records["driver"]["as"]
    assert before.records["driver"]["cpu"] == after.records["driver"]["cpu"]
    assert before.records["driver"]["descriptors"] == after.records["driver"]["descriptors"]
    creations = r"\b(?:clone|clone3|fork|vfork)\("
    assert re.findall(creations, before.syscalls), "no process-creation observations"
    assert re.findall(creations, before.syscalls) == re.findall(creations, after.syscalls)
    executions = r'\bexecve\("([^"]+)"'
    assert re.findall(executions, before.syscalls), "no exec observations"
    assert re.findall(executions, before.syscalls) == re.findall(executions, after.syscalls)
    assert before.records["driver"]["stdout"] == before.records["driver"]["stderr"]
    assert after.records["driver"]["stdout"] == after.records["driver"]["stderr"]
    assert Path(after.records["driver"]["parent_exe"]).samefile(shutil.which("bash"))
    assert after.argv.split(b"\0")[:-1] == [
        b"-std=c++17", b"-O2", b"-ffp-contract=off", b"-fPIC", b"-shared",
        os.fsencode(f"-I{box.prefix}/include"), b"-I/usr/include/eigen3",
        os.fsencode(box.indir / "strategy.cpp"),
        b"-Wl,--whole-archive", os.fsencode(box.prefix / "lib/libpineforge.a"),
        b"-Wl,--no-whole-archive", b"-o", os.fsencode(box.root / "work/strategy.so")]


def test_explicit_limits_allow_an_ordinary_success_without_changing_compile_argv(box):
    before = box.run(entrypoint=box.reference)
    after = box.run({WALL: "10000", ADDRESS: str(2 << 30), DIAGNOSTIC: str(1 << 20)})
    assert before.returncode == after.returncode == 0, (before.stderr, after.stderr)
    assert before.stdout == after.stdout
    assert before.stderr == after.stderr
    assert before.argv == after.argv


@pytest.mark.parametrize("limits", [{}, {WALL: "10000", ADDRESS: str(2 << 30),
                                        DIAGNOSTIC: str(1 << 20)}])
def test_real_syntax_failure_keeps_exit_three_and_the_original_bytes(box, limits):
    with (box.indir / "strategy.cpp").open("ab") as source:
        source.write(b"\nthis is deliberately invalid C++;\n")
    before = box.run(entrypoint=box.reference)
    after = box.run(limits)
    assert before.returncode == after.returncode == 3, (before.stderr, after.stderr)
    assert before.stdout == after.stdout == b""
    assert before.stderr == after.stderr
    assert b"this is deliberately invalid C++;" in after.stderr, after.stderr
    assert b"fatal error:" not in after.stderr, after.stderr
    assert after.stderr.endswith(COMPILE_FAILURE)
    assert not (box.root / "run-started").exists()


@pytest.mark.parametrize("mode", ["hang", "orphan"])
def test_absolute_wall_stop_reaps_detached_helpers_even_after_driver_exit(box, mode):
    wall_ms = 800
    result = box.run({WALL: str(wall_ms), "PF_ENTRYPOINT_FIXTURE_MODE": mode})
    assert_failure(result, "run_deadline_exceeded", {"stage": "compile"})
    assert_tree_gone(result)


@pytest.mark.parametrize("cap", [1 << 30, 2 << 30])
def test_address_limit_is_read_back_in_driver_and_execed_helper(box, cap):
    result = box.run({ADDRESS: str(cap), "PF_ENTRYPOINT_FIXTURE_MODE": "limits"})
    assert result.returncode == 0, result.stderr
    assert result.records["driver"]["as"] == [cap, cap]
    assert result.records["limits_helper"]["as"] == [cap, cap]
    assert result.stdout == b'{"signals":[2.0,-3.0]}\n'


def test_actual_mapping_denial_keeps_compiler_failure_without_an_invented_cause(box):
    cap = 128 << 20
    requested = 512 << 20
    result = box.run({ADDRESS: str(cap), "PF_ENTRYPOINT_FIXTURE_MODE": "memory",
                      "PF_ENTRYPOINT_FIXTURE_ALLOCATION": str(requested)})
    assert result.returncode == 3, (result.stdout, result.stderr)
    assert result.stdout == b""
    assert result.stderr == COMPILE_LOG + COMPILE_FAILURE
    assert result.records["driver"]["as"] == [cap, cap]
    assert result.records["memory"]["as"] == [cap, cap]
    allocation = result.records["allocation"]
    assert allocation["requested"] == requested
    assert allocation.get("errno") == errno.ENOMEM
    assert "allocated" not in allocation
    assert not (box.root / "run-started").exists()


@pytest.mark.parametrize("cap", [17, 4096])
@pytest.mark.parametrize("stream", ["stdout", "stderr", "mixed"])
@pytest.mark.parametrize("mode", ["flood", "flood_tree"])
def test_diagnostic_overflow_reads_one_probe_and_retains_only_the_cap(box, cap, stream, mode):
    payload = bytes(range(256)) * ((cap * 4 + 256) // 256)
    result = box.run({DIAGNOSTIC: str(cap), "PF_ENTRYPOINT_FIXTURE_MODE": mode,
                      "PF_ENTRYPOINT_FIXTURE_STREAM": stream,
                      "PF_ENTRYPOINT_FIXTURE_SPLIT_AT": str(cap // 2),
                      "PF_ENTRYPOINT_FIXTURE_DIAGNOSTIC": box.payload(payload)},
                     trace_reads=True)
    assert_failure(result, "diagnostic_budget_exceeded",
                   {"cap_bytes": cap, "used_bytes": cap, "requested_bytes": 1})
    assert result.stderr == COMPILE_LOG + payload[:cap]
    assert pipe_read_bytes(result) == cap + 1
    if mode == "flood_tree":
        assert_tree_gone(result)


@pytest.mark.parametrize("stream", ["stdout", "stderr", "mixed"])
@pytest.mark.parametrize("cap", [17, 768, 65536, 65537])
def test_exact_diagnostic_fit_is_success_and_keeps_raw_binary_bytes(box, stream, cap):
    payload = (bytes(range(256)) * ((cap + 255) // 256))[:cap]
    result = box.run({DIAGNOSTIC: str(len(payload)),
                      "PF_ENTRYPOINT_FIXTURE_STREAM": stream,
                      "PF_ENTRYPOINT_FIXTURE_DIAGNOSTIC": box.payload(payload)},
                     trace_reads=True)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert result.stdout == b'{"signals":[2.0,-3.0]}\n'
    assert result.stderr == COMPILE_LOG + payload + b"[pineforge] running backtest ...\n"
    assert pipe_read_bytes(result) == len(payload)


@pytest.mark.parametrize("knob", [WALL, ADDRESS, DIAGNOSTIC])
@pytest.mark.parametrize("value", ["", "+1", "-1", " 1", "1 ", "1\n", "1.0", "1e3",
                                  "0x10", "\u0661", str(MAXIMUM + 1), "9" * 80])
def test_malformed_knobs_are_usage_errors_before_any_compiler_starts(box, knob, value):
    result = box.run({knob: value, "PF_ENTRYPOINT_FIXTURE_MODE": "reject_probe"})
    assert_failure(result, "run_request_invalid", {"option": "arguments"})
    assert "driver" not in result.records
    reason = (b"is outside 0..9007199254740990" if value in (str(MAXIMUM + 1), "9" * 80)
              else b"must be an unsigned decimal integer in 0..9007199254740990")
    assert result.stderr == (COMPILE_LOG + b"entrypoint.sh: error: "
                             + os.fsencode(knob) + b" " + reason + b"\n")


@pytest.mark.parametrize("knob", [WALL, DIAGNOSTIC])
def test_largest_representable_positive_ceiling_does_not_create_a_new_refusal(box, knob):
    result = box.run({knob: str(MAXIMUM), "PF_ENTRYPOINT_FIXTURE_MODE": "reject_probe"})
    assert result.returncode == 3, (result.stdout, result.stderr)
    assert result.stdout == b""
    assert "driver" in result.records
    assert result.stderr == COMPILE_LOG + PROBE_FAILURE + COMPILE_FAILURE
    assert not (box.root / "run-started").exists()


def test_address_request_above_an_inherited_hard_limit_is_refused_without_clamping(box):
    inherited = 512 << 20
    result = box.run({ADDRESS: str(inherited + 1),
                      "PF_ENTRYPOINT_FIXTURE_MODE": "reject_probe"}, inherited_as=inherited)
    assert_failure(result, "run_request_invalid", {"option": "arguments"})
    assert "driver" not in result.records
    assert os.fsencode(ADDRESS) in result.stderr
    assert b"exceeds the inherited hard limit" in result.stderr


@pytest.mark.parametrize("inherited", [None, 2 << 30])
def test_zero_address_ceiling_is_unlimited_and_preserves_inherited_limit(box, inherited):
    result = box.run({ADDRESS: "0", "PF_ENTRYPOINT_FIXTURE_MODE": "limits"},
                     inherited_as=inherited)
    expected = (list(resource.getrlimit(resource.RLIMIT_AS))
                if inherited is None else [inherited, inherited])
    if inherited is None:
        assert expected == [resource.RLIM_INFINITY, resource.RLIM_INFINITY]
    assert result.returncode == 0, result.stderr
    assert result.records["driver"]["as"] == expected
    assert result.records["limits_helper"]["as"] == expected
    assert result.stdout == b'{"signals":[2.0,-3.0]}\n'
    assert result.stderr == COMPILE_LOG + b"[pineforge] running backtest ...\n"


def test_zero_diagnostic_ceiling_streams_a_payload_larger_than_one_mib(box):
    minimum = 1 << 20
    payload = bytes(range(256)) * (minimum // 256 + 1)
    result = box.run({WALL: "10000", DIAGNOSTIC: "0",
                      "PF_ENTRYPOINT_FIXTURE_MODE": "flood",
                      "PF_ENTRYPOINT_FIXTURE_DIAGNOSTIC": box.payload(payload)},
                     trace_reads=True)
    assert result.returncode == 3, (result.stdout, result.stderr)
    assert result.stdout == b""
    assert result.stderr == COMPILE_LOG + payload + COMPILE_FAILURE
    assert len(payload) > minimum
    assert pipe_read_bytes(result) == len(payload)


def test_zero_wall_ceiling_keeps_the_frozen_direct_process_table(box):
    before = box.run(entrypoint=box.reference, trace_processes=True)
    after = box.run({WALL: "0"}, trace_processes=True)
    assert before.returncode == after.returncode == 0, (before.stderr, after.stderr)
    assert before.stdout == after.stdout == b'{"signals":[2.0,-3.0]}\n'
    assert before.stderr == after.stderr
    assert before.argv == after.argv
    before_environment = set(before.environment.rstrip(b"\0").split(b"\0"))
    after_environment = set(after.environment.rstrip(b"\0").split(b"\0"))
    assert after_environment - before_environment == {b"PINEFORGE_COMPILE_WALL_MS=0"}
    assert before_environment <= after_environment
    assert before.records["driver"]["parent_exe"] == after.records["driver"]["parent_exe"]
    assert before.records["driver"]["descriptors"] == after.records["driver"]["descriptors"]
    assert before.records["driver"]["stdout"] == before.records["driver"]["stderr"]
    assert after.records["driver"]["stdout"] == after.records["driver"]["stderr"]


def test_zero_guard_path_validates_a_malformed_sibling_before_compiler(box):
    result = box.run({WALL: "0", ADDRESS: "not-a-number",
                      "PF_ENTRYPOINT_FIXTURE_MODE": "reject_probe"})
    assert_failure(result, "run_request_invalid", {"option": "arguments"})
    assert "driver" not in result.records
    assert result.stderr == (
        COMPILE_LOG + b"entrypoint.sh: error: " + os.fsencode(ADDRESS)
        + b" must be an unsigned decimal integer in 0..9007199254740990\n")


@pytest.mark.parametrize("nonblocking", [False, True])
@pytest.mark.parametrize("outcome", ["success", "compiler_failure", "overflow", "deadline"])
def test_guard_restores_the_callers_stderr_flags(box, nonblocking, outcome):
    extra = {DIAGNOSTIC: "4096"}
    if outcome == "compiler_failure":
        extra["PF_ENTRYPOINT_FIXTURE_MODE"] = "reject_probe"
    elif outcome == "overflow":
        extra.update({DIAGNOSTIC: "17", "PF_ENTRYPOINT_FIXTURE_MODE": "flood",
                      "PF_ENTRYPOINT_FIXTURE_DIAGNOSTIC": box.payload(b"x" * 18)})
    elif outcome == "deadline":
        extra.update({WALL: "800", "PF_ENTRYPOINT_FIXTURE_MODE": "orphan"})
    with (box.root / "stderr-output").open("wb", buffering=0) as sink:
        flags = fcntl.fcntl(sink, fcntl.F_GETFL)
        if nonblocking:
            flags |= os.O_NONBLOCK
        fcntl.fcntl(sink, fcntl.F_SETFL, flags)
        result = box.run(extra, stderr_sink=sink)
        assert fcntl.fcntl(sink, fcntl.F_GETFL) == flags
    if outcome == "success":
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert result.stdout == b'{"signals":[2.0,-3.0]}\n'
    elif outcome == "compiler_failure":
        assert result.returncode == 3, (result.stdout, result.stderr)
        assert result.stdout == b""
        assert result.stderr == COMPILE_LOG + PROBE_FAILURE + COMPILE_FAILURE
        assert not (box.root / "run-started").exists()
    elif outcome == "overflow":
        assert_failure(result, "diagnostic_budget_exceeded",
                       {"cap_bytes": 17, "used_bytes": 17, "requested_bytes": 1})
        assert result.stderr == COMPILE_LOG + b"x" * 17
    else:
        assert_failure(result, "run_deadline_exceeded", {"stage": "compile"})
        assert_tree_gone(result)


def test_blocked_compile_failure_message_still_honors_the_wall_deadline(box):
    wall_ms = 1500
    strace = shutil.which("strace")
    assert strace, "strace is required to observe the compiler's actual exit"
    command = [strace, "-f", "-qq", "-e", "trace=process",
               "-o", str(box.trace / "syscalls.log"), "bash", str(ENTRYPOINT)]
    env = {**box.env, WALL: str(wall_ms), "PF_ENTRYPOINT_FIXTURE_MODE": "gated_failure"}
    read_fd, write_fd = os.pipe()
    with os.fdopen(read_fd, "rb", buffering=0) as reader, \
            os.fdopen(write_fd, "wb", buffering=0) as writer:
        flags = fcntl.fcntl(writer, fcntl.F_GETFL) | os.O_NONBLOCK
        fcntl.fcntl(writer, fcntl.F_SETFL, flags)
        started = time.monotonic()
        process = subprocess.Popen(command, env=env, cwd=box.root, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=writer, start_new_session=True)
        box.processes.append(process)
        while not (box.trace / "failure-ready").is_file():
            assert process.poll() is None, "entrypoint exited before the compiler was ready"
            time.sleep(0.005)
        assert (box.trace / "failure-ready").is_file(), "compiler never reached the release gate"
        assert fcntl.fcntl(writer, fcntl.F_GETFL) & os.O_NONBLOCK
        pressure_bytes = 0
        while True:
            try:
                pressure_bytes += os.write(writer.fileno(), b"x" * 4096)
            except BlockingIOError:
                break
        assert pressure_bytes > 0
        (box.trace / "release-failure").write_bytes(b"exit")
        stdout, _ = process.communicate(input=b"")
        elapsed = time.monotonic() - started
        assert fcntl.fcntl(writer, fcntl.F_GETFL) == flags
        os.set_blocking(reader.fileno(), False)
        stderr = bytearray()
        while True:
            try:
                block = os.read(reader.fileno(), 65536)
            except BlockingIOError:
                break
            if not block:
                break
            stderr.extend(block)
    result = Invocation(subprocess.CompletedProcess(
        command, process.returncode, stdout, bytes(stderr)), elapsed, box.trace)
    assert_failure(result, "run_deadline_exceeded", {"stage": "compile"})
    assert result.stderr == COMPILE_LOG + b"x" * pressure_bytes
    driver = result.records["driver"]
    assert re.search(rf"(?m)^{driver['pid']}\s+exit_group\(17(?:\)|\s)",
                     result.syscalls), result.syscalls
    assert not original_process_present(driver)


@pytest.fixture
def selected_box(box):
    # Build the real module once here; the driver fixture then installs those
    # bytes promptly, leaving the entire short deadline for the binder fixture.
    artifact = box.root / "linked-strategy.so"
    subprocess.run([box.real_gxx, "-std=c++17", "-O2", "-ffp-contract=off", "-fPIC", "-shared",
                    f"-I{box.prefix}/include", "-I/usr/include/eigen3",
                    str(box.indir / "strategy.cpp"), "-Wl,--whole-archive",
                    str(box.prefix / "lib/libpineforge.a"), "-Wl,--no-whole-archive",
                    "-o", str(artifact)], check=True, capture_output=True)
    inventory = box.root / "source-inventory.json"
    inventory.write_text('{"fixture":"binder lifetime"}\n', encoding="utf-8")
    box.env.update({"PINEFORGE_REPORT_POLICY": "selected-window/v1",
                    "PINEFORGE_REQUEST_FEED_INVENTORY": str(inventory),
                    "PF_ENTRYPOINT_FIXTURE_MODE": "artifact",
                    "PF_ENTRYPOINT_FIXTURE_ARTIFACT": str(artifact)})
    return box


def test_binder_uses_the_same_absolute_compile_deadline(selected_box):
    result = selected_box.run({WALL: "3000", "PF_ENTRYPOINT_FIXTURE_BINDER": "hang",
                               "PF_ENTRYPOINT_FIXTURE_DELAY_MS": "2100"})
    assert "binder" in result.records, result.stderr
    assert_failure(result, "run_deadline_exceeded", {"stage": "compile"})
    assert not original_process_present(result.records["binder"])


def test_binder_shares_the_compiler_diagnostic_allowance(selected_box):
    prefix = b"compiler diagnostic\n"
    suffix = b"binder diagnostic\n" * 128
    cap = len(prefix) + 17
    result = selected_box.run({
        DIAGNOSTIC: str(cap), "PF_ENTRYPOINT_FIXTURE_BINDER": "flood",
        "PF_ENTRYPOINT_FIXTURE_DIAGNOSTIC": selected_box.payload(prefix, "compiler-output"),
        "PF_ENTRYPOINT_FIXTURE_BINDER_DIAGNOSTIC": selected_box.payload(suffix, "binder-output"),
    }, trace_reads=True)
    assert "binder" in result.records, result.stderr
    assert_failure(result, "diagnostic_budget_exceeded",
                   {"cap_bytes": cap, "used_bytes": cap, "requested_bytes": 1})
    assert result.stderr == COMPILE_LOG + prefix + BIND_LOG + suffix[:cap - len(prefix)]
    assert pipe_read_bytes(result) + pipe_read_bytes(result, "binder") == cap + 1


@pytest.mark.parametrize("limits", [{}, {WALL: "10000", ADDRESS: str(2 << 30)}])
def test_genuine_binder_failure_keeps_exit_three(selected_box, limits):
    result = selected_box.run({**limits, "PF_ENTRYPOINT_FIXTURE_BINDER": "fail"})
    assert result.returncode == 3, (result.stdout, result.stderr)
    assert result.stdout == b""
    assert "binder" in result.records
    assert result.stderr == (COMPILE_LOG + BIND_LOG + BINDER_FAILURE
                             + b"[pineforge] request-feed inventory binding failed\n")
    assert not (selected_box.root / "run-started").exists()


@pytest.mark.parametrize("binder_payload", [b"", bytes(range(256))])
def test_binder_exact_fit_shares_the_compiler_allowance_and_succeeds(selected_box,
                                                                  binder_payload):
    prefix = b"compiler diagnostic\n"
    cap = len(prefix) + len(binder_payload)
    result = selected_box.run({
        DIAGNOSTIC: str(cap), "PF_ENTRYPOINT_FIXTURE_BINDER": "success",
        "PF_ENTRYPOINT_FIXTURE_DIAGNOSTIC": selected_box.payload(prefix, "compiler-output"),
        "PF_ENTRYPOINT_FIXTURE_BINDER_DIAGNOSTIC":
            selected_box.payload(binder_payload, "binder-output"),
    }, trace_reads=True)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert result.stdout == b'{"signals":[2.0,-3.0]}\n'
    assert result.stderr == (COMPILE_LOG + prefix + BIND_LOG + binder_payload
                             + b"[pineforge] running backtest ...\n")
    assert pipe_read_bytes(result) + pipe_read_bytes(result, "binder") == cap
