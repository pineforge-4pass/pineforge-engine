"""Linux statfs controls for S4E. No row skips; run only on the lab box."""
import ctypes
import json
import os
from pathlib import Path
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json
import test_run_json_sink as ordinary


MEMORY_FILESYSTEMS = {0x01021994: "tmpfs", 0x858458F6: "ramfs"}


def _kind(path):
    """Independent statfs input check, not the harness's directory selector."""
    libc = ctypes.CDLL(None, use_errno=True)
    result = ctypes.create_string_buffer(256)
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        if libc.fstatfs(fd, ctypes.byref(result)) != 0:
            raise OSError(ctypes.get_errno(), "test fstatfs")
        return ctypes.c_long.from_buffer(result).value
    finally:
        os.close(fd)


@pytest.mark.parametrize("backing", ["available", "none", "probe-error", "spill"])
@pytest.mark.parametrize("kind", list(MEMORY_FILESYSTEMS), ids=["tmpfs", "ramfs"])
@pytest.mark.parametrize("outputs", [False, True], ids=["default", "output-columns"])
def test_default_run_with_tmpfs_tmpdir(
        tmp_path, monkeypatch, record_property, backing, kind, outputs):
    shm = Path("/dev/shm")
    if shm.is_dir() and _kind(shm) == kind:
        # This case uses a real Linux tmpfs (normally /dev/shm).
        memory = shm
        simulated = False
    else:
        # No matching mount: simulate ONLY libc's statfs output below.
        memory = tmp_path / "memory-tmpdir"
        memory.mkdir()
        simulated = True
    record_property("filesystem_case", "simulated statfs input" if simulated else "real mount")
    monkeypatch.delenv("PINEFORGE_RUN_JSON_PROFILE", raising=False)
    monkeypatch.setenv("TMPDIR", str(memory))
    monkeypatch.setattr(tempfile, "tempdir", None)
    assert Path(tempfile.gettempdir()) == memory
    spill = tmp_path / "spill"
    spill.mkdir()
    real_cdll = ctypes.CDLL
    probes = []
    memory_path, fallback_path = memory.resolve(), Path("/var/tmp").resolve()

    class FilesystemInput:
        def __init__(self, *args, **kwargs):
            self.libc = real_cdll(*args, **kwargs)

        def __getattr__(self, name):
            return getattr(self.libc, name)

        def fstatfs(self, fd, output):
            path = Path(os.readlink(f"/proc/self/fd/{fd}").removesuffix(" (deleted)"))
            probes.append(path)
            in_memory = path == memory_path or memory_path in path.parents
            candidate = in_memory or path == fallback_path
            if backing == "probe-error" and candidate:
                ctypes.set_errno(5)  # EIO: uncertainty must never authorize the engine path.
                return -1
            if (simulated and in_memory) or (backing == "none" and candidate):
                ctypes.cast(output, ctypes.POINTER(ctypes.c_long))[0] = kind
                return 0
            return self.libc.fstatfs(fd, output)

    # Selection, tempfile creation, sink setup and serialization all stay live.
    monkeypatch.setattr(run_json.ctypes, "CDLL", FilesystemInput)
    artifacts = []
    real_mkstemp, real_temporary = tempfile.mkstemp, tempfile.TemporaryFile

    def mkstemp(*args, **kwargs):
        artifacts.append(Path(kwargs["dir"]))
        return real_mkstemp(*args, **kwargs)

    def temporary(*args, **kwargs):
        artifacts.append(Path(kwargs["dir"] or tempfile.gettempdir()))
        return real_temporary(*args, **kwargs)

    monkeypatch.setattr(tempfile, "mkstemp", mkstemp)
    monkeypatch.setattr(tempfile, "TemporaryFile", temporary)

    class EnginePaths(ordinary.SinkLibrary):
        def install(self, state, pointer):
            descriptor = pointer._obj
            self.scratch_directory = Path(os.fsdecode(descriptor.scratch_dir))
            artifacts.append(self.scratch_directory)
            return super().install(state, pointer)

    library = EnginePaths(outputs=outputs)
    extra = ["--spill-dir", str(spill)] if backing == "spill" else []
    if outputs:
        extra.append("--outputs")
    status, stdout, stderr, _ = ordinary.invoke(
        tmp_path, monkeypatch, library=library, extra=extra)
    assert probes, "the harness must reach the real filesystem-type probe"
    assert stderr == "", stdout
    if status == 0:
        assert backing not in {"none", "probe-error"}, stdout
        assert artifacts and library.installed
        if backing == "spill":
            # F-3: an explicit directory WINS; a run only succeeds when it is usable.
            assert _kind(spill) not in MEMORY_FILESYSTEMS and os.access(
                spill, os.W_OK | os.X_OK), stdout
        selected = spill if backing == "spill" else Path("/var/tmp")
        assert all(path == selected for path in artifacts), artifacts
        assert all(path != memory and memory not in path.parents for path in artifacts)
        assert _kind(selected) not in MEMORY_FILESYSTEMS
        assert Path(library.installed[0]).parent == selected
        assert json.loads(stdout)["summary"]["total_trades"] == 3
    else:
        failure = json.loads(stdout)
        assert (status, failure["code"]) == (1, "run_request_invalid"), failure
        assert "TMPDIR" in failure["error"] and "--spill-dir" in failure["error"]
        assert "disk-backed" in failure["error"]
        assert ("tmpfs" in failure["error"] or "ramfs" in failure["error"]
                or "probe" in failure["error"])
        assert artifacts == [] and library.installed == []
        if backing in {"available", "spill"}:
            # F-3: with an explicit directory only that directory is a candidate.
            candidates = [spill] if backing == "spill" else [Path("/var/tmp")]
            for selected in candidates:
                if selected.is_dir():
                    assert _kind(selected) in MEMORY_FILESYSTEMS or not os.access(
                        selected, os.W_OK | os.X_OK), failure
