#!/usr/bin/env python3
"""Real compiler driver, with controllable resource-consuming child fixtures."""

import errno
import fcntl
import json
import mmap
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import sys
import time


TRACE = Path(os.environ["PF_ENTRYPOINT_FIXTURE_TRACE"])


def record(role):
    pid = os.getpid()
    descriptors = {}
    for spelling in os.listdir("/proc/self/fd"):
        fd = int(spelling)
        try:
            info = os.fstat(fd)
            descriptors[spelling] = {
                "type": stat.S_IFMT(info.st_mode),
                "flags": fcntl.fcntl(fd, fcntl.F_GETFL),
                "cloexec": fcntl.fcntl(fd, fcntl.F_GETFD),
            }
        except OSError as error:
            if error.errno != errno.EBADF:
                raise
    try:
        parent_exe = os.readlink(f"/proc/{os.getppid()}/exe")
    except FileNotFoundError:
        parent_exe = None
    document = {
        "pid": pid,
        "start_ticks": Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()[19],
        "parent_exe": parent_exe,
        "as": list(resource.getrlimit(resource.RLIMIT_AS)),
        "cpu": list(resource.getrlimit(resource.RLIMIT_CPU)),
        "descriptors": descriptors,
        "stdout": os.readlink("/proc/self/fd/1"),
        "stderr": os.readlink("/proc/self/fd/2"),
    }
    (TRACE / f"{role}.json").write_text(json.dumps(document), encoding="utf-8")
    return document


def emit(payload):
    channel = os.environ.get("PF_ENTRYPOINT_FIXTURE_STREAM", "stderr")
    parts = [(1 if channel == "stdout" else 2, payload)]
    if channel == "mixed":
        middle = int(os.environ.get("PF_ENTRYPOINT_FIXTURE_SPLIT_AT", len(payload) // 2))
        parts = [(1, payload[:middle]), (2, payload[middle:])]
    for fd, part in parts:
        offset = 0
        while offset < len(part):
            offset += os.write(fd, part[offset:offset + 8192])


def detached_helper():
    ready_read, ready_write = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(ready_read)
        os.setsid()
        record("middle")
        grandchild = os.fork()
        if grandchild:
            os._exit(0)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        record("helper")
        os.close(1)
        os.close(2)
        with (TRACE / "heartbeat").open("ab", buffering=0) as heartbeat:
            heartbeat.write(time.monotonic_ns().to_bytes(8, "little"))
            os.write(ready_write, b"r")
            os.close(ready_write)
            while True:
                heartbeat.write(time.monotonic_ns().to_bytes(8, "little"))
                time.sleep(0.02)
    os.close(ready_write)
    if os.read(ready_read, 1) != b"r":
        raise RuntimeError("helper failed to start")
    os.close(ready_read)


def hanging_tree(orphan):
    detached_helper()
    if orphan:
        os._exit(0)
    while True:
        emit(b"progress\n")
        time.sleep(0.05)


def memory_child():
    record("memory")
    requested = int(os.environ["PF_ENTRYPOINT_FIXTURE_ALLOCATION"])
    try:
        region = mmap.mmap(-1, requested)
        for offset in range(0, requested, mmap.PAGESIZE):
            region[offset] = 1
        (TRACE / "allocation.json").write_text(
            json.dumps({"requested": requested, "allocated": len(region)}), encoding="utf-8")
    except (OSError, MemoryError) as error:
        (TRACE / "allocation.json").write_text(
            json.dumps({"requested": requested, "errno": getattr(error, "errno", None),
                        "exception": type(error).__name__}), encoding="utf-8")
        return 42
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--read-limits"]:
        record("limits_helper")
    elif sys.argv[1:] == ["--allocate"]:
        sys.exit(memory_child())
    else:
        record("driver")
        (TRACE / "argv.bin").write_bytes(
            b"\0".join(os.fsencode(value) for value in sys.argv[1:]) + b"\0")
        (TRACE / "environment.bin").write_bytes(Path("/proc/self/environ").read_bytes())
        mode = os.environ.get("PF_ENTRYPOINT_FIXTURE_MODE", "compile")
        if mode in ("hang", "orphan"):
            hanging_tree(mode == "orphan")
        elif mode == "memory":
            result = subprocess.run([sys.executable, __file__, "--allocate"], check=False)
            sys.exit(result.returncode)
        elif mode == "reject_probe":
            emit(b"fixture compiler: requested probe failure\n")
            sys.exit(17)
        elif mode == "gated_failure":
            (TRACE / "failure-ready").write_bytes(b"ready")
            while not (TRACE / "release-failure").is_file():
                time.sleep(0.005)
            sys.exit(17)
        else:
            if mode == "flood_tree":
                detached_helper()
            delay_ms = os.environ.get("PF_ENTRYPOINT_FIXTURE_DELAY_MS")
            if delay_ms is not None:
                time.sleep(int(delay_ms) / 1000)
            if mode == "limits":
                subprocess.run([sys.executable, __file__, "--read-limits"], check=True)
            diagnostic = os.environ.get("PF_ENTRYPOINT_FIXTURE_DIAGNOSTIC")
            if diagnostic:
                emit(Path(diagnostic).read_bytes())
            if mode in ("flood", "flood_tree"):
                sys.exit(23)
            if mode == "artifact":
                output = Path(sys.argv[sys.argv.index("-o") + 1])
                output.write_bytes(Path(os.environ["PF_ENTRYPOINT_FIXTURE_ARTIFACT"]).read_bytes())
            else:
                compiler = os.environ["PF_ENTRYPOINT_FIXTURE_GXX"]
                os.execv(compiler, [compiler, *sys.argv[1:]])
