#!/usr/bin/env python3
"""Keep Linux memory-filesystem magic values in the canonical predicate file."""

import os
from pathlib import Path
import re
import sys


ROOT = Path(__file__).absolute().parent.parent
CANONICAL = ROOT / "include/pineforge/detail/scratch_directory.hpp"
MAGIC = re.compile(rb"0x(?:01021994|858458f6)", re.IGNORECASE)


def refuse_unsafe_path(path: Path) -> None:
    # Refuse ~/.config/zsh/keys, ~/.config/pineforge, ~/.ssh, ~/.aws,
    # ~/.codex and every *.env by name, before opening or descending.
    if any(part in {".config", ".ssh", ".aws", ".codex"} for part in path.parts):
        raise RuntimeError(f"refused protected path: {path}")
    if path.name.endswith(".env"):
        raise RuntimeError(f"refused environment file: {path}")
    if path.is_symlink():
        raise RuntimeError(f"refused symlink: {path}")


def main() -> int:
    files = []
    def walk_error(error: OSError) -> None:
        raise error

    for subtree in ("src", "include"):
        start = ROOT / subtree
        refuse_unsafe_path(start)
        if not start.is_dir():
            raise RuntimeError(f"missing source directory: {start}")
        for directory, children, names in os.walk(start, followlinks=False, onerror=walk_error):
            for name in sorted(children):
                refuse_unsafe_path(Path(directory) / name)
            for name in sorted(names):
                path = Path(directory) / name
                refuse_unsafe_path(path)
                if not path.is_file():
                    raise RuntimeError(f"refused non-regular source: {path}")
                files.append(path)

    # List the entire read set before reading any file contents.
    for path in sorted(files):
        print(f"S4E inspection path: {path}", flush=True)
    failures = []
    for path in sorted(files):
        refuse_unsafe_path(path)
        contents = path.read_bytes()
        if path == CANONICAL:
            literals = [match.lower() for match in MAGIC.findall(contents)]
            for literal in (b"0x01021994", b"0x858458f6"):
                count = literals.count(literal)
                if count != 1:
                    failures.append(
                        f"{path.relative_to(ROOT)}: {literal.decode()} occurs {count} times, expected 1"
                    )
            continue
        for number, line in enumerate(contents.splitlines(), 1):
            if MAGIC.search(line):
                failures.append(f"{path.relative_to(ROOT)}:{number}")
    if failures:
        print("memory-filesystem literals must occur once each in scratch_directory.hpp only:\n"
              + "\n".join(failures), file=sys.stderr)
        return 1
    print("test_scratch_filesystem_predicate: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"test_scratch_filesystem_predicate: {error}", file=sys.stderr)
        sys.exit(1)
