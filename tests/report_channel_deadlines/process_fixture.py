"""Separate real-process fixtures; no syscall replacements enter the module."""

import dataclasses
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import sys
import time


MODULE = Path(__file__).resolve().parents[2] / "docker/pineforge_report_deadlines.py"


def payload(length, seed):
    block = bytes((index * 37 + seed) % 256 for index in range(256))
    return (block * ((length + 255) // 256))[:length]


def emit(value):
    print(json.dumps(value), flush=True)


def start():
    emit({"ready": True})
    if sys.stdin.readline().strip() != "go":
        raise RuntimeError("fixture did not receive its start command")


def load_module(path=MODULE):
    path = Path(path)
    if not path.is_file():
        raise AssertionError(
            "main has no docker/pineforge_report_deadlines.py; "
            "the report deadline tests must fail before implementation"
        )
    spec = importlib.util.spec_from_file_location("pineforge_report_deadlines", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def descriptor_live(fd):
    if fd is None:
        return None
    try:
        os.fstat(fd)
        return True
    except OSError:
        return False


def worker(config):
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    signals_seen = []
    if config.get("signal_handler"):
        def handle_signal(signum, frame):
            signals_seen.append(signum)
        signal.signal(signal.SIGUSR1, handle_signal)
        signal.siginterrupt(signal.SIGUSR1, True)
    module = load_module(config.get("module_path", MODULE))
    data = payload(config.get("size", 0), config.get("seed", 0))
    start()
    deadline = module.Deadline(config.get("wall_ms", 1000),
                               config.get("stall_ms", 500))
    received = bytearray()
    try:
        operation = config["operation"]
        if operation in ("write", "idle_write"):
            idle_expired = None
            if operation == "idle_write":
                time.sleep(config["idle_ms"] / 1000)
                idle_expired = deadline.expired()
            result = {"written": module.bounded_write(
                config["fd"], data, deadline, config["stage_hint"]),
                      "idle_expired": idle_expired}
        elif operation in ("read", "read_many"):
            eof = False
            while True:
                part, eof = module.bounded_read(
                    config["fd"], config["nmax"], deadline, config["stage_hint"])
                received.extend(part)
                if operation == "read" or eof:
                    break
            result = {"data_hex": received.hex(), "eof": eof}
        elif operation == "relay":
            result = dataclasses.asdict(module.relay(
                config["src_fd"], config["fd"], config["allowance"], deadline,
                config["stage_hint"], chunk=config.get("chunk", 65536)))
        elif operation == "close":
            success = module.close_once(config["fd"])
            try:
                os.fstat(config["fd"])
                live, fd_errno = True, None
            except OSError as exc:
                live, fd_errno = False, exc.errno
            result = {"success": success, "live": live, "fd_errno": fd_errno}
        elif operation == "deadline":
            time.sleep(config["progress_after_ms"] / 1000)
            wall_before = deadline.remaining_wall_ms()
            stall_before = deadline.stall_remaining_ms()
            deadline.note_progress(0)
            stall_zero = deadline.stall_remaining_ms()
            deadline.note_progress(config["progress_bytes"])
            result = {
                "wall_before": wall_before,
                "wall_after": deadline.remaining_wall_ms(),
                "stall_before": stall_before,
                "stall_zero": stall_zero,
                "stall_positive": deadline.stall_remaining_ms(),
            }
            while not deadline.expired():
                # This fixture progresses while the independent wall still runs.
                deadline.note_progress(config["progress_bytes"])
                time.sleep(0.01)
            result["remaining_at_expiry"] = deadline.remaining_wall_ms()
        else:
            raise ValueError("unknown fixture operation")
        emit({"ok": True, "result": result, "signals_seen": len(signals_seen),
              "wall_remaining_ms": deadline.remaining_wall_ms(),
              "stall_remaining_ms": deadline.stall_remaining_ms(),
              "wall_expired": deadline.expired(),
              "sigpipe": int(signal.getsignal(signal.SIGPIPE)),
              "fd_live": descriptor_live(config.get("fd")),
              "src_live": descriptor_live(config.get("src_fd"))})
    except (module.DeadlineError, module.ChannelIOError) as exc:
        result = {
            "ok": False, "exception": type(exc).__name__,
            "kind": getattr(exc, "kind", None),
            "errno": getattr(exc, "errno", None),
            "stage_hint": exc.stage_hint, "bytes_moved": exc.bytes_moved,
            "signals_seen": len(signals_seen), "data_hex": received.hex(),
            "sigpipe": int(signal.getsignal(signal.SIGPIPE)),
            "fd_live": descriptor_live(config.get("fd")),
            "src_live": descriptor_live(config.get("src_fd")),
        }
        if getattr(exc, "relay_result", None) is not None:
            result["relay_result"] = dataclasses.asdict(exc.relay_result)
        emit(result)
    if config.get("signal_handler"):
        # Keep handlers installed until the external sender has stopped.
        if sys.stdin.readline().strip() != "release":
            raise RuntimeError("signal fixture did not receive its release command")


def constructors(config):
    module = load_module()
    start()
    snapshots = []
    for wall_ms, stall_ms in ((None, None), (None, 0), (0, None), (0, 0),
                              (None, 700), (0, 700), (700, None), (700, 0),
                              (0.0, 0.0)):
        deadline = module.Deadline(wall_ms, stall_ms)
        deadline.note_progress(0)
        deadline.note_progress(1)
        snapshots.append({
            "wall_ms": wall_ms, "stall_ms": stall_ms,
            "wall_remaining_ms": deadline.remaining_wall_ms(),
            "stall_remaining_ms": deadline.stall_remaining_ms(),
            "expired": deadline.expired(),
        })
    invalid = (
        ("negative", -1), ("negative_fraction", -0.5),
        ("true", True), ("false", False),
        ("nan", float("nan")), ("infinity", float("inf")),
        ("negative_infinity", float("-inf")), ("infinity_text", "Infinity"),
        ("negative_infinity_text", "-inf"), ("nan_text", "NaN"),
        ("numeric_text", "150"), ("zero_text", "0"),
        ("bytes", b"150"), ("complex", 150 + 0j), ("list", []),
        ("mapping", {}), ("object", object()),
        ("unrepresentable", 9007199254740991),
    )
    rejections = []
    for name, value in invalid:
        for dimension in ("wall", "stall"):
            args = (value, None) if dimension == "wall" else (None, value)
            try:
                module.Deadline(*args)
            except Exception as exc:
                failure = type(exc).__name__
            else:
                failure = "accepted"
            rejections.append({"name": name, "dimension": dimension,
                               "exception": failure})
    emit({"snapshots": snapshots, "rejections": rejections})


def reader(config):
    endpoint = socket.socket(fileno=config["fd"])
    start()
    mode = config.get("mode", "drain")
    if mode in ("never", "after_backpressure"):
        if sys.stdin.readline().strip() != "drain":
            raise RuntimeError("reader did not receive its drain command")
    count = 0
    digest = hashlib.sha256()
    read_times = []
    while True:
        maximum = 1 if mode == "bytewise" else 8192
        if "close_after" in config:
            maximum = min(maximum, config["close_after"] - count)
            if maximum == 0:
                break
        part = endpoint.recv(maximum)
        if not part:
            break
        count += len(part)
        digest.update(part)
        if mode == "bytewise":
            read_times.append(time.monotonic())
            time.sleep(config["interval_ms"] / 1000)
    if config.get("reset"):
        endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii", 1, 0))
    endpoint.close()
    emit({"count": count, "sha256": digest.hexdigest(), "read_times": read_times})


def writer(config):
    endpoint = socket.socket(fileno=config["fd"])
    data = payload(config["size"], config["seed"])
    start()
    count = 0
    while count < len(data):
        try:
            moved = endpoint.send(data[count:count + config.get("step", 8192)])
        except (BrokenPipeError, ConnectionResetError):
            break
        if moved <= 0:
            raise RuntimeError("fixture writer made no progress")
        count += moved
        if config.get("interval_ms"):
            time.sleep(config["interval_ms"] / 1000)
    endpoint.close()
    emit({"count": count, "sha256": hashlib.sha256(data[:count]).hexdigest()})


def storm(config):
    start()
    sent = 0
    commands = select.poll()
    commands.register(sys.stdin, select.POLLIN)
    expiry = time.monotonic() + config["duration_ms"] / 1000
    while time.monotonic() < expiry:
        if commands.poll(0):
            if sys.stdin.readline().strip() != "stop":
                raise RuntimeError("signal sender received an invalid command")
            break
        try:
            os.kill(config["pid"], signal.SIGUSR1)
        except ProcessLookupError:
            break
        sent += 1
        time.sleep(config["interval_ms"] / 1000)
    emit({"sent": sent})


def import_probe(config):
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    before = {
        "sigpipe": signal.getsignal(signal.SIGPIPE),
        "sigusr1": signal.getsignal(signal.SIGUSR1),
        "files": sorted(os.listdir(".")),
        "fds": sorted(os.listdir("/proc/self/fd")),
        "tasks": sorted(os.listdir("/proc/self/task")),
    }
    load_module()
    after = {
        "sigpipe": signal.getsignal(signal.SIGPIPE),
        "sigusr1": signal.getsignal(signal.SIGUSR1),
        "files": sorted(os.listdir(".")),
        "fds": sorted(os.listdir("/proc/self/fd")),
        "tasks": sorted(os.listdir("/proc/self/task")),
    }
    start()
    emit({"before": before, "after": after})


if __name__ == "__main__":
    roles = {"worker": worker, "reader": reader, "writer": writer,
             "storm": storm, "import": import_probe, "constructors": constructors}
    roles[sys.argv[1]](json.loads(sys.argv[2]))
