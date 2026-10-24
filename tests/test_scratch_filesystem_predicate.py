#!/usr/bin/env python3
"""Keep Linux memory-filesystem magic values in the canonical predicate file."""

import os
from pathlib import Path
import re
import shutil
import sys
import tempfile


ROOT = Path(__file__).absolute().parent.parent
CANONICAL = ROOT / "include/pineforge/detail/scratch_directory.hpp"
MAGIC = re.compile(
    rb"\b(?:TMPFS_MAGIC|RAMFS_MAGIC|0x0*(?:1021994|858458f6)[ul]*"
    rb"|(?:16914836|2240043254)[ul]*)\b",
    re.IGNORECASE,
)


def refuse_unsafe_path(path: Path) -> None:
    # Refuse ~/.config/zsh/keys, ~/.config/pineforge, ~/.ssh, ~/.aws,
    # ~/.codex and every *.env by name, before opening or descending.
    if any(part in {".config", ".ssh", ".aws", ".codex"} for part in path.parts):
        raise RuntimeError(f"refused protected path: {path}")
    if path.name.endswith(".env"):
        raise RuntimeError(f"refused environment file: {path}")
    if path.is_symlink():
        raise RuntimeError(f"refused symlink: {path}")


def scan(root: Path) -> int:
    refuse_unsafe_path(root)
    canonical = root / CANONICAL.relative_to(ROOT)
    files = []
    def walk_error(error: OSError) -> None:
        raise error

    for subtree in ("src", "include"):
        start = root / subtree
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
        # C++ digit separators do not change a value. Keep scanning comments
        # and strings as well as code, including named Linux magic macros.
        contents = re.sub(rb"(?<=[0-9a-fA-F])'(?=[0-9a-fA-F])", b"", path.read_bytes())
        if path == canonical:
            literals = [match.lower() for match in MAGIC.findall(contents)]
            for literal in (b"0x01021994", b"0x858458f6"):
                count = literals.count(literal)
                if count != 1:
                    failures.append(
                        f"{path.relative_to(root)}: {literal.decode()} occurs {count} times, expected 1"
                    )
            if len(literals) != 2:
                failures.append(
                    f"{path.relative_to(root)}: {len(literals)} memory magic spellings, expected 2"
                )
            continue
        for number, line in enumerate(contents.splitlines(), 1):
            if MAGIC.search(line):
                failures.append(f"{path.relative_to(root)}:{number}")
    if failures:
        print("memory-filesystem literals must occur once each in scratch_directory.hpp only:\n"
              + "\n".join(failures), file=sys.stderr)
        return 1
    print(f"test_scratch_filesystem_predicate scan: PASS ({root})")
    return 0


def self_test() -> int:
    temporary_root = Path(tempfile.gettempdir())
    refuse_unsafe_path(temporary_root)
    parent = Path(tempfile.mkdtemp(prefix="predicate-self-test-"))
    refuse_unsafe_path(parent)
    created = parent.stat()
    try:
        print(f"S4E self-test fixture root: {parent}", flush=True)
        return self_test_trees(parent)
    finally:
        refuse_unsafe_path(parent)
        current = parent.stat()
        if (parent.resolve().parent != temporary_root.resolve()
                or not parent.name.startswith("predicate-self-test-")
                or (current.st_dev, current.st_ino) != (created.st_dev, created.st_ino)):
            raise RuntimeError(f"refused cleanup of unowned self-test directory: {parent}")
        shutil.rmtree(parent)


def self_test_trees(parent: Path) -> int:
    canonical_bytes = b"0x01021994\n0x858458f6\n"

    def make_tree(name: str, spelling: bytes, in_canonical: bool = False) -> Path:
        tree = parent / name
        canonical = tree / CANONICAL.relative_to(ROOT)
        source = tree / "src/probe.cpp"
        for path in (tree, canonical, source):
            refuse_unsafe_path(path)
        canonical.parent.mkdir(parents=True)
        source.parent.mkdir()
        canonical.write_bytes(canonical_bytes + (spelling + b"\n" if in_canonical else b""))
        source.write_bytes(b"// " + (b"clean source" if in_canonical else spelling) + b"\n")
        return tree

    if scan(make_tree("clean", b"clean source")) != 0:
        raise RuntimeError("self-test clean tree must pass")
    spellings = (
        b"TMPFS_MAGIC", b"RAMFS_MAGIC",
        b"0x01021994", b"0x858458f6", b"0X01021994", b"0X858458F6",
        b"0x1021994", b"0x0000001021994", b"0x0000858458f6",
        b"0x0102'1994", b"0x8584'58f6", b"0x0'102'1994",
        b"16914836", b"2240043254", b"16914836UL", b"2240043254ULL",
        b"16'914'836", b"2'240'043'254",
    )
    for number, spelling in enumerate(spellings):
        for in_canonical in (False, True):
            location = "canonical" if in_canonical else "source"
            tree = make_tree(f"{number:02d}-{location}", spelling, in_canonical)
            if scan(tree) != 1:
                raise RuntimeError(
                    f"self-test missed {spelling.decode()} in {location}: {tree}"
                )
    print("test_scratch_filesystem_predicate_self_test: PASS")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if sys.argv[1:]:
        raise RuntimeError("usage: test_scratch_filesystem_predicate.py [--self-test]")
    return scan(ROOT)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"test_scratch_filesystem_predicate: {error}", file=sys.stderr)
        sys.exit(1)
