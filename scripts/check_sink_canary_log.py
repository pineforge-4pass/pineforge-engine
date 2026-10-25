#!/usr/bin/env python3
"""Collect sweep canaries and check coverage against the probes actually run.

Append only ^sink_(canary|kept): lines, from stdin or completed probe logs:
    python3 -I scripts/check_sink_canary_log.py append LOG [OUTPUT ...]
Check exactly one successful canary per probe, with no failed, malformed,
duplicate or unexpected canaries:
    python3 -I scripts/check_sink_canary_log.py check LOG --probes DIR ...
    python3 -I scripts/check_sink_canary_log.py check LOG --probes-file FILE
Add --subset-summary to print the successful count from the same parse.
Check exactly one kept artifact per requested probe, confined to DIRECTORY,
and print each file's whole-file SHA-256 and byte count:
    python3 -I scripts/check_sink_canary_log.py kept LOG --directory DIRECTORY --probes DIR ...

Probe files contain one directory/id per line, with optional # comments.
The sweep, not a copied population count, supplies the expectation.
Only the sweep truncates LOG, once before running probes; append never does.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
from pathlib import Path
import re
import sys


CANARY = re.compile(
    r"sink_canary: probe=(?P<probe>\S+) bytes=\d+ records=\d+ trades=\d+"
    r" equity=\d+ (?P<verdict>ok|FAIL(?: .*)?)")
PROBE = re.compile(r"^sink_canary: probe=(\S+)")
KEPT = re.compile(r"sink_kept: probe=(?P<probe>\S+) path=(?P<path>.+)")


def append_lines(log: Path, lines) -> None:
    """Preserve passing and failing canaries and kept paths, and nothing else."""
    with log.open("a", encoding="utf-8", newline="\n") as output:
        for line in lines:
            if line.startswith(("sink_canary:", "sink_kept:")):
                output.write(line.rstrip("\r\n") + "\n")


def probe_id(value: str) -> str:
    return "_".join(Path(value).name.split()) or "-"


def inspect_log(log: Path, probes: list[str]) -> tuple[list[str], int]:
    """Return coverage diagnostics and the successful count from one parse."""
    expected = Counter(probe_id(probe) for probe in probes)
    seen = Counter()
    successful = 0
    problems = []
    if not expected:
        problems.append("the sweep supplied no probes")
    for probe, count in expected.items():
        if count != 1:
            problems.append(f"probe={probe}: repeated in the sweep's probe list")
    with log.open(encoding="utf-8") as source:
        for number, line in enumerate(source, 1):
            line = line.rstrip("\r\n")
            if line.startswith("sink_kept:"):
                continue
            found = CANARY.fullmatch(line)
            name = PROBE.match(line)
            probe = name[1] if name else f"<unknown at line {number}>"
            if not found:
                problems.append(f"probe={probe}: malformed canary at line {number}")
                continue
            seen[probe] += 1
            if probe not in expected:
                problems.append(f"probe={probe}: not in the sweep's probe list")
            if found["verdict"] != "ok":
                problems.append(f"probe={probe}: {found['verdict']}")
            else:
                successful += 1
    for probe in expected:
        if seen[probe] != 1:
            problems.append(f"probe={probe}: expected one canary, found {seen[probe]}")
    return problems, successful


def check_log(log: Path, probes: list[str]) -> list[str]:
    """Return diagnostics naming every missing, repeated or invalid probe."""
    return inspect_log(log, probes)[0]


def check_kept(log: Path, probes: list[str], directory: Path
               ) -> tuple[list[str], list[tuple[str, str, int]]]:
    """Hash actual kept files; never trust a digest or count from the log."""
    expected = Counter(probe_id(probe) for probe in probes)
    seen = Counter()
    paths = set()
    artifacts = {}
    problems = []
    if not expected:
        problems.append("the sweep requested no kept sinks")
    for probe, count in expected.items():
        if count != 1:
            problems.append(f"probe={probe}: repeated in the kept probe list")
    directory = directory.resolve(strict=True)
    if not directory.is_dir():
        return [f"kept sink directory is not a directory: {directory}"], []
    with log.open(encoding="utf-8") as source:
        for number, line in enumerate(source, 1):
            if not line.startswith("sink_kept:"):
                continue
            found = KEPT.fullmatch(line.rstrip("\r\n"))
            if not found:
                problems.append(f"malformed kept sink at line {number}")
                continue
            probe = found["probe"]
            seen[probe] += 1
            if probe not in expected:
                problems.append(f"probe={probe}: not in the kept probe list")
                continue
            try:
                path = Path(found["path"]).resolve(strict=True)
                if path.parent != directory or not path.is_file():
                    problems.append(f"probe={probe}: kept sink is not a file in {directory}")
                    continue
                if path in paths:
                    problems.append(f"probe={probe}: kept sink path is repeated")
                    continue
                paths.add(path)
                digest = hashlib.sha256()
                size = 0
                with path.open("rb") as sink:
                    while block := sink.read(1024 * 1024):
                        digest.update(block)
                        size += len(block)
                if size == 0:
                    problems.append(f"probe={probe}: kept sink is empty")
                    continue
                artifacts[probe] = (probe, digest.hexdigest(), size)
            except OSError as error:
                problems.append(f"probe={probe}: cannot read kept sink: {error}")
    for probe in expected:
        if seen[probe] != 1:
            problems.append(f"probe={probe}: expected one kept sink, found {seen[probe]}")
    return problems, [artifacts[probe] for probe in expected if probe in artifacts]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    append = modes.add_parser("append", help="append only canary/kept lines")
    append.add_argument("log", type=Path)
    append.add_argument("outputs", type=Path, nargs="*")
    check = modes.add_parser("check", help="check coverage for the probes run")
    check.add_argument("log", type=Path)
    check.add_argument("--subset-summary", action="store_true",
                       help="print the parsed sink-on subset canary count")
    kept = modes.add_parser("kept", help="check and hash the requested kept sinks")
    kept.add_argument("log", type=Path)
    kept.add_argument("--directory", type=Path, required=True)
    for mode in (check, kept):
        source = mode.add_mutually_exclusive_group(required=True)
        source.add_argument("--probes", nargs="*")
        source.add_argument("--probes-file", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.mode == "append":
            if args.outputs:
                for path in args.outputs:
                    with path.open(encoding="utf-8", errors="replace") as lines:
                        append_lines(args.log, lines)
            else:
                append_lines(args.log, sys.stdin)
            return 0
        probes = args.probes
        if args.probes_file is not None:
            probes = [
                entry for line in args.probes_file.read_text(encoding="utf-8").splitlines()
                if (entry := line.partition("#")[0].strip())
            ]
        if args.mode == "kept":
            problems, artifacts = check_kept(args.log, probes, args.directory)
        else:
            problems, successful = inspect_log(args.log, probes)
    except (OSError, UnicodeError) as error:
        print(f"sink_canary_log: {error}", file=sys.stderr)
        return 1
    if problems:
        for problem in problems:
            print(f"sink_canary_log: {problem}", file=sys.stderr)
        return 1
    if args.mode == "kept":
        print(f"sink-on kept sinks: {len(artifacts)}")
        for probe, digest, size in artifacts:
            print(f"sink_kept: probe={probe} sha256={digest} bytes={size}")
    else:
        print(f"sink_canary_log: ok probes={len(probes)}")
        if args.subset_summary:
            print(f"sink-on subset: {successful} sink_canary ok lines for {len(probes)} probes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
