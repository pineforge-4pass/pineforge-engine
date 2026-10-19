#!/usr/bin/env python3
"""Run the real target in this process; record its clock and live OHLCV VMAs.

No RSS sampling: the parent still obtains this process's peak with wait4.
Only mmap construction is observed, so there is no per-call/profile hook in
the backtest or serializer. Every captured segment comes from /proc/PID/maps.
"""
from __future__ import annotations

import argparse
import json
import mmap
import os
from pathlib import Path
import runpy
import sys
import time


def refuse_name(value) -> None:
    if isinstance(value, int) or value is None:
        return
    path = Path(os.path.abspath(os.fsdecode(value)))
    text = str(path)
    if (path.name.endswith(".env")
            or any(part in {".ssh", ".aws", ".codex"} for part in path.parts)
            or "/.config/zsh/keys" in text or "/.config/pineforge" in text):
        raise PermissionError(f"REFUSED sensitive path by name: {path}")


def checked(value) -> Path:
    refuse_name(value)
    path = Path(value).resolve()
    refuse_name(path)
    return path


def audit(event, args):
    if event in {"open", "ctypes.dlopen", "os.listdir", "os.scandir"} and args:
        refuse_name(args[0])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--run-json", type=Path, required=True)
    parser.add_argument("target_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    manifest = checked(args.manifest)
    target = checked(args.run_json)
    output = checked(args.output)
    print(f"FOLLOW {manifest}", file=sys.stderr, flush=True)
    document = json.loads(manifest.read_text(encoding="utf-8"))
    expected = {str(checked(item["path"])) for item in document["mapped_files"]}
    proc_path = Path(f"/proc/{os.getpid()}/maps")
    for path in (target, output, proc_path, *sorted(expected)):
        print(f"FOLLOW {path}", file=sys.stderr, flush=True)
    forwarded = args.target_args
    if forwarded[:1] == ["--"]:
        forwarded = forwarded[1:]
    target_argv = [str(target), *forwarded]
    snapshots = []
    start = 0

    def capture(reason):
        # Read only the process map file. Never open/follow paths found in it.
        entries = []
        per_file = {}
        for line in proc_path.read_text(encoding="utf-8").splitlines():
            fields = line.split(None, 5)
            if len(fields) != 6:
                continue
            backing = fields[5].removesuffix(" (deleted)")
            if backing not in expected:
                continue
            first, last = fields[0].split("-")
            length = int(last, 16) - int(first, 16)
            entry = {"file": backing, "start": first, "end": last,
                     "mapped_bytes": length, "permissions": fields[1],
                     "offset": fields[2], "device": fields[3],
                     "inode": fields[4], "proc_maps_line": line}
            entries.append(entry)
            per_file[backing] = per_file.get(backing, 0) + length
        snapshots.append({"reason": reason, "pid": os.getpid(),
            "source": str(proc_path), "child_clock_ns": time.perf_counter_ns(),
            "files": per_file, "segments": entries,
            "total_mapped_bytes": sum(per_file.values())})

    original = mmap.mmap

    class ObservedMap(original):
        def __new__(cls, *values, **keywords):
            instance = super().__new__(cls, *values, **keywords)
            capture("after-mmap-created")
            return instance

    mmap.mmap = ObservedMap
    sys.addaudithook(audit)
    sys.path.insert(0, str(target.parent))
    sys.argv = target_argv
    target_returncode = 0
    start = time.perf_counter_ns()
    try:
        capture("before-target")
        runpy.run_path(str(target), run_name="__main__")
    except SystemExit as error:
        target_returncode = error.code if isinstance(error.code, int) else 0 if error.code is None else 1
        raise
    except BaseException:
        target_returncode = 1
        raise
    finally:
        sys.stdout.flush()
        sys.stderr.flush()
        ended = time.perf_counter_ns()
        capture("after-target")
        mmap.mmap = original
        largest = max(snapshots, key=lambda item: item["total_mapped_bytes"])
        missing = sorted(expected - set(largest["files"]))
        payload = {"schema": "pineforge-flat-rss-child-probe/v1",
            "pid": os.getpid(), "target_argv": target_argv,
            "target_returncode": target_returncode,
            "clock_source": "child perf_counter_ns around runpy.run_path plus target stdout flush",
            "clock_scope": "target imports, input load, backtest, report/serialization; excludes probe bootstrap and probe-result write",
            "child_wall_started_ns": start, "child_wall_ended_ns": ended,
            "child_wall_seconds": (ended - start) / 1e9,
            "mapped_source": "live /proc/PID/maps address-range lengths for declared OHLCV files",
            "mapped_kind": "virtual mapped bytes, including page rounding; not resident RSS",
            "expected_files": document["mapped_files"],
            "mapping_complete": not missing, "missing_files": missing,
            "real_mapped_ohlcv_bytes": largest["total_mapped_bytes"],
            "selected_live_snapshot": largest, "snapshots": snapshots}
        with output.open("x", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True, allow_nan=False)
            handle.write("\n")


if __name__ == "__main__":
    main()
