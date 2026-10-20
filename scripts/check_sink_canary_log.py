#!/usr/bin/env python3
"""Collect sweep canaries and check coverage against the probes actually run.

Append only ^sink_(canary|kept): lines, from stdin or completed probe logs:
    python3 -I scripts/check_sink_canary_log.py append LOG [OUTPUT ...]
Check exactly one successful canary per probe, with no failed, malformed,
duplicate or unexpected canaries:
    python3 -I scripts/check_sink_canary_log.py check LOG --probes DIR ...
    python3 -I scripts/check_sink_canary_log.py check LOG --probes-file FILE

Probe files contain one directory/id per line, with optional # comments.
The sweep, not a copied population count, supplies the expectation.
Only the sweep truncates LOG, once before running probes; append never does.
"""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import sys


CANARY = re.compile(
    r"sink_canary: probe=(?P<probe>\S+) bytes=\d+ records=\d+ trades=\d+"
    r" equity=\d+ (?P<verdict>ok|FAIL(?: .*)?)")
PROBE = re.compile(r"^sink_canary: probe=(\S+)")


def append_lines(log: Path, lines) -> None:
    """Preserve passing and failing canaries and kept paths, and nothing else."""
    with log.open("a", encoding="utf-8", newline="\n") as output:
        for line in lines:
            if line.startswith(("sink_canary:", "sink_kept:")):
                output.write(line.rstrip("\r\n") + "\n")


def probe_id(value: str) -> str:
    return "_".join(Path(value).name.split()) or "-"


def check_log(log: Path, probes: list[str]) -> list[str]:
    """Return diagnostics naming every missing, repeated or invalid probe."""
    expected = Counter(probe_id(probe) for probe in probes)
    seen = Counter()
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
    for probe in expected:
        if seen[probe] != 1:
            problems.append(f"probe={probe}: expected one canary, found {seen[probe]}")
    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    append = modes.add_parser("append", help="append only canary/kept lines")
    append.add_argument("log", type=Path)
    append.add_argument("outputs", type=Path, nargs="*")
    check = modes.add_parser("check", help="check coverage for the probes run")
    check.add_argument("log", type=Path)
    source = check.add_mutually_exclusive_group(required=True)
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
        problems = check_log(args.log, probes)
    except (OSError, UnicodeError) as error:
        print(f"sink_canary_log: {error}", file=sys.stderr)
        return 1
    if problems:
        for problem in problems:
            print(f"sink_canary_log: {problem}", file=sys.stderr)
        return 1
    print(f"sink_canary_log: ok probes={len(probes)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
