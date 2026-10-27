"""Real transactional report I/O, with forwarding syscall observations."""
from contextlib import ExitStack
import ctypes
import errno
import hashlib
import os
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
import run_json


WINDOW = 64 * 1024
READ_LIMIT = 1024 * 1024


class _IOCall:
    """Forward ctypes declarations as well as calls to the actual libc."""

    def __init__(self, function, name, probe):
        self.function, self.name, self.probe = function, name, probe

    @property
    def argtypes(self):
        return self.function.argtypes

    @argtypes.setter
    def argtypes(self, value):
        self.function.argtypes = value

    @property
    def restype(self):
        return self.function.restype

    @restype.setter
    def restype(self, value):
        self.function.restype = value

    def __call__(self, fd, address, count, offset):
        calls = self.probe.calls[self.name]
        calls.append((fd, count, offset))
        attempt, fault = self.probe.faults.get(self.name, (0, None))
        if len(calls) == attempt:
            self.probe.hits.append((self.name, fault))
            if fault == "short":
                # Still perform a real, aligned, short write.
                return self.function(fd, address, count // 2, offset)
            if fault == "zero":
                return 0
            ctypes.set_errno(errno.EINTR if fault == "eintr" else errno.EIO)
            return -1
        return self.function(fd, address, count, offset)


class _Libc:
    def __init__(self, library, probe):
        self.library = library
        self.pread = _IOCall(library.pread, "pread", probe)
        self.pwrite = _IOCall(library.pwrite, "pwrite", probe)

    def __getattr__(self, name):
        return getattr(self.library, name)


class _IOProbe:
    """Observe OS boundaries; do not replace any report/artifact method."""

    def __init__(self, monkeypatch):
        self.calls = {"pread": [], "pwrite": []}
        self.truncates, self.syncs, self.faults, self.hits = [], [], {}, []
        self.fail_sync = self.fail_truncate = False
        cdll = ctypes.CDLL
        truncate = os.ftruncate

        def library(*args, **kwargs):
            return _Libc(cdll(*args, **kwargs), self)

        def ftruncate(fd, size):
            self.truncates.append((fd, size))
            if self.fail_truncate:
                raise OSError(errno.EIO, os.strerror(errno.EIO))
            return truncate(fd, size)

        monkeypatch.setattr(run_json.ctypes, "CDLL", library)
        monkeypatch.setattr(run_json.os, "ftruncate", ftruncate)
        for name in ("fdatasync", "fsync"):
            if not hasattr(os, name):
                continue
            function = getattr(os, name)

            def sync(fd, *, function=function, name=name):
                self.syncs.append((name, fd, os.fstat(fd).st_size))
                if self.fail_sync:
                    raise OSError(errno.EIO, os.strerror(errno.EIO))
                return function(fd)

            monkeypatch.setattr(run_json.os, name, sync)


@pytest.fixture
def probe(monkeypatch):
    return _IOProbe(monkeypatch)


def _bytes(size, start=0):
    # Independent, nonuniform ASCII, including zero bytes and block seams.
    alphabet = bytes(range(128))
    return (alphabet[start:] + alphabet[:start]) * (size // 128) + (
        alphabet[start:] + alphabet[:start])[:size % 128]


def _report(resources, tmp_path, size):
    # No escaping is needed for this alphabet: the complete wire bytes are
    # specified here, independently of the streaming encoder.
    alphabet = b"abcdefghijklmnopqrstuvwxyz_0123456789-"
    payload = (alphabet * (size // len(alphabet) + 1))[:size]
    expected = b'{"payload":"' + payload + b'"}\n'
    args = SimpleNamespace(profile="unjailed", spill_dir=str(tmp_path))
    artifact = run_json._prepare_report_json(
        {"payload": payload.decode("ascii")}, resources, args, None)
    return artifact, expected


def _drain(artifact, request=READ_LIMIT):
    actual = bytearray()
    digest = hashlib.sha256()
    while chunk := artifact.read(request):
        assert 0 < len(chunk) <= min(request, READ_LIMIT)
        assert len(artifact.buffer) <= READ_LIMIT
        raw = chunk if isinstance(chunk, bytes) else chunk.encode("ascii")
        actual.extend(raw)
        digest.update(raw)
    return bytes(actual), digest.digest()


def _assert_closed(fd):
    with pytest.raises(OSError) as error:
        os.fstat(fd)
    assert error.value.errno == errno.EBADF


@pytest.mark.parametrize("sync_mode", ("available", "fsync"))
def test_report_syscall_counts_and_bounded_readback(tmp_path, monkeypatch, probe, sync_mode):
    if sync_mode == "fsync":
        monkeypatch.delattr(run_json.os, "fdatasync", raising=False)
    assert run_json._BoundedJSONFile.window == WINDOW
    with ExitStack() as resources:
        artifact, expected = _report(resources, tmp_path, 2 * READ_LIMIT + 17)
        full_windows = len(expected) // WINDOW
        assert full_windows >= 10
        assert artifact.size == os.fstat(artifact.fileno()).st_size == len(expected)
        assert not list(tmp_path.iterdir())  # private, already unlinked
        assert artifact.read(0) == ""
        # Match the public report publisher's bounded requests.
        actual, digest = _drain(artifact)
        assert actual == expected
        assert digest == hashlib.sha256(expected).digest()
        writes, reads = probe.calls["pwrite"], probe.calls["pread"]
        assert len(writes) == (len(expected) + WINDOW - 1) // WINDOW
        assert sum(count == WINDOW for _, count, _ in writes) == full_windows
        assert reads and all(0 < count <= READ_LIMIT for _, count, _ in reads)
        assert all(count % 4096 == offset % 4096 == 0 for _, count, offset in reads)
        assert len(reads) == (len(expected) + READ_LIMIT - 1) // READ_LIMIT
        assert len(probe.truncates) <= 1
        assert len(probe.syncs) == 1
        assert probe.syncs[0][2] == len(expected)
        if sync_mode == "fsync":
            assert probe.syncs[0][0] == "fsync"
        # Rewind and over-large/default requests retain the public 1 MiB cap.
        artifact.seek(0)
        assert artifact.read(3 * READ_LIMIT).encode("ascii") == expected[:READ_LIMIT]
        assert artifact.read().encode("ascii") == expected[READ_LIMIT:2 * READ_LIMIT]
        assert artifact.read(-7).encode("ascii") == expected[2 * READ_LIMIT:]
        assert artifact.read() == ""
        artifact.binary = True
        artifact.seek(0)
        actual, digest = _drain(artifact)
        assert actual == expected
        assert digest == hashlib.sha256(expected).digest()
        assert artifact.read(0) == b""
        assert all(count <= READ_LIMIT for _, count, _ in probe.calls["pread"])
        assert len(probe.syncs) == 1
    assert artifact.closed and artifact.buffer is None


def test_byte_identity_at_block_and_window_edges_without_reflush(tmp_path, probe):
    sizes = (0, 1, 4095, 4096, 4097, WINDOW - 1, WINDOW, WINDOW + 1, 3 * WINDOW + 17)
    repeated = []
    for size in sizes:
        expected = _bytes(size)
        with ExitStack() as resources:
            artifact = run_json._json_temporary(resources, str(tmp_path))
            start = (len(probe.calls["pwrite"]), len(probe.truncates), len(probe.syncs))
            for offset in range(0, size, 7001):
                artifact.write(expected[offset:offset + 7001].decode("ascii"))
            artifact.flush()
            assert artifact.size == os.fstat(artifact.fileno()).st_size == size
            before = (len(probe.calls["pwrite"]), len(probe.truncates), len(probe.syncs))
            artifact.flush()
            artifact.seek(0)
            after = (len(probe.calls["pwrite"]), len(probe.truncates), len(probe.syncs))
            repeated.append((size, start, before, after))
            assert artifact.read(0) == ""
            actual, digest = _drain(artifact, 4095)
            assert actual == expected
            assert digest == hashlib.sha256(expected).digest()
            assert artifact.position == size
    # Check all byte cases before the optimization assertion. Before the
    # change, partial tails are rewritten/truncated/synced at every rewind.
    assert all(before == after for _, _, before, after in repeated), repeated
    assert all(before[2] - start[2] == int(size > 0)
               for size, start, before, _ in repeated), repeated
    assert all(before[1] - start[1] == int(size % 4096 != 0)
               for size, start, before, _ in repeated), repeated


def test_partial_flush_then_append_never_publishes_padding(tmp_path, probe):
    initial, suffix = _bytes(3 * WINDOW + 17), _bytes(WINDOW + 2, 71)
    expected = initial + suffix
    with ExitStack() as resources:
        artifact = run_json._json_temporary(resources, str(tmp_path))
        artifact.write(initial.decode("ascii"))
        artifact.flush()
        assert artifact.size == os.fstat(artifact.fileno()).st_size == len(initial)
        artifact.flush()
        artifact.write(suffix.decode("ascii"))
        artifact.flush()
        assert artifact.size == os.fstat(artifact.fileno()).st_size == len(expected)
        artifact.seek(0)
        actual, digest = _drain(artifact)
        assert len(actual) == len(expected)
        assert actual == expected
        assert digest == hashlib.sha256(expected).digest()
        assert len(probe.calls["pwrite"]) == 6  # four full windows, two partial flushes
        assert len(probe.truncates) == 2
        assert [size for _, _, size in probe.syncs] == [len(initial), len(expected)]


@pytest.mark.parametrize("fault", ("short", "eio"))
def test_write_refusal_after_real_windows_preserves_oserror(tmp_path, probe, fault):
    probe.faults["pwrite"] = (11, fault)
    with ExitStack() as resources:
        with pytest.raises(OSError) as error:
            _report(resources, tmp_path, 12 * WINDOW + 17)
        expected = ("short report artifact write" if fault == "short"
                    else str(OSError(errno.EIO, os.strerror(errno.EIO))))
        assert type(error.value) is OSError
        assert str(error.value) == expected
        assert probe.hits == [("pwrite", fault)]
        writes = probe.calls["pwrite"]
        assert len(writes) == 11
        assert [offset for _, count, offset in writes[:10] if count == WINDOW] == [
            index * WINDOW for index in range(10)]
        _assert_closed(writes[-1][0])
        assert not list(tmp_path.iterdir())
        assert probe.syncs == []  # no final flush was reached
        assert probe.truncates == []


@pytest.mark.parametrize("fault", ("zero", "eio"))
def test_read_refusal_preserves_oserror_after_one_final_sync(tmp_path, probe, fault):
    with ExitStack() as resources:
        artifact, expected = _report(resources, tmp_path, 12 * WINDOW + 17)
        assert len(expected) // WINDOW >= 10
        probe.faults["pread"] = (1, fault)
        with pytest.raises(OSError) as error:
            artifact.read()
        text = ("short report artifact read" if fault == "zero"
                else str(OSError(errno.EIO, os.strerror(errno.EIO))))
        assert type(error.value) is OSError
        assert str(error.value) == text
        assert probe.hits == [("pread", fault)]
        assert len(probe.syncs) == 1
        assert len(probe.truncates) <= 1


@pytest.mark.parametrize("sync_mode", ("available", "fsync"))
def test_final_sync_eio_is_reported_before_publication(tmp_path, monkeypatch, probe, sync_mode):
    if sync_mode == "fsync":
        monkeypatch.delattr(run_json.os, "fdatasync", raising=False)
    probe.fail_sync = True
    with ExitStack() as resources:
        with pytest.raises(OSError) as error:
            _report(resources, tmp_path, 12 * WINDOW + 17)
        assert type(error.value) is OSError
        assert str(error.value) == str(OSError(errno.EIO, os.strerror(errno.EIO)))
        assert len(probe.syncs) == 1
        _, fd, size_at_sync = probe.syncs[0]
        assert len(probe.calls["pwrite"]) >= 10
        assert size_at_sync == 12 * WINDOW + 17 + len(b'{"payload":""}\n')
        _assert_closed(fd)
        assert not list(tmp_path.iterdir())


def test_tail_truncate_eio_refuses_publication(tmp_path, probe):
    probe.fail_truncate = True
    with ExitStack() as resources:
        with pytest.raises(OSError) as error:
            _report(resources, tmp_path, 12 * WINDOW + 17)
        assert type(error.value) is OSError
        assert str(error.value) == str(OSError(errno.EIO, os.strerror(errno.EIO)))
        assert len(probe.truncates) == 1
        fd, size = probe.truncates[0]
        assert size == 12 * WINDOW + 17 + len(b'{"payload":""}\n')
        assert len(probe.calls["pwrite"]) >= 10
        assert probe.syncs == []
        _assert_closed(fd)
        assert not list(tmp_path.iterdir())


def test_eintr_retries_real_io_and_retains_single_sync(tmp_path, probe):
    probe.faults = {"pwrite": (1, "eintr"), "pread": (1, "eintr")}
    with ExitStack() as resources:
        artifact, expected = _report(resources, tmp_path, 12 * WINDOW + 17)
        assert len(expected) // WINDOW >= 10
        actual, digest = _drain(artifact)
        assert actual == expected
        assert digest == hashlib.sha256(expected).digest()
        assert probe.hits == [("pwrite", "eintr"), ("pread", "eintr")]
        assert probe.calls["pwrite"][0] == probe.calls["pwrite"][1]
        assert probe.calls["pread"][0] == probe.calls["pread"][1]
        assert len(probe.syncs) == 1
        assert len(probe.truncates) <= 1
