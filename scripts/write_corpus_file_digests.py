#!/usr/bin/env python3
"""Write the complete corpus file digest manifest from a clean Git checkout.

The manifest records what the corpus held when it was written. It detects
changes without a rewrite and makes deliberate changes visible in its diff.
It is not a signature. Exit 0: written; 1: refused; 2: could not run.
"""

import json
import os
from pathlib import Path
import sys


sys.dont_write_bytecode = True
from validate_corpus_sha import (  # noqa: E402
    FILE_DIGESTS, FILE_DIGEST_SCHEMA, _Parser, _git, _local_file, _mode_problem,
    _normalized_file_path, _require_materialized, _sha256, _tracked_entries,
)


def _check_clean(root: Path, outputs: set[str]) -> None:
    rows = iter(_git(root, "status", "--porcelain=v1", "-z",
                     "--untracked-files=all").split(b"\0"))
    for row in rows:
        if not row:
            continue
        if len(row) < 4 or row[2:3] != b" ":
            raise ValueError(f"{root}: unreadable Git status entry {row!r}")
        flags = row[:2]
        names = [os.fsdecode(row[3:])]
        if b"R" in flags or b"C" in flags:
            previous = next(rows, b"")
            if not previous:
                raise ValueError(f"{root}: incomplete Git rename status")
            names.append(os.fsdecode(previous))
        for name in names:
            if name not in outputs:
                raise ValueError(f"{root / name}: checkout is dirty ({flags.decode('ascii')})")


def write_file_digests(corpus: Path, output: Path | None = None) -> Path:
    """Hash tracked regular files in chunks and write only the selected output."""
    root = corpus.resolve()
    top = Path(os.fsdecode(_git(root, "rev-parse", "--show-toplevel")).strip()).resolve()
    if top != root:
        raise ValueError(f"{root}: --corpus must name the Git checkout root")
    _git(root, "rev-parse", "--verify", "HEAD^{commit}")
    destination = output if output is not None else root / FILE_DIGESTS
    if destination.is_symlink():
        raise ValueError(f"{destination}: manifest output must not be a symlink")
    destination = destination.resolve()

    entries = _tracked_entries(root)
    tracked = set()
    for name, mode, stage in entries:
        problem = _mode_problem(mode)
        if problem:
            raise ValueError(f"{root / name}: {problem}")
        if stage != "0":
            raise ValueError(f"{root / name}: unresolved index stage {stage}")
        if not _normalized_file_path(name):
            raise ValueError(f"{root / name}: unusable tracked path {name!r}")
        tracked.add(name)
    if not tracked - {FILE_DIGESTS}:
        raise ValueError(f"{root}: no tracked corpus files to record")

    outputs = {FILE_DIGESTS}
    if destination.is_relative_to(root):
        name = destination.relative_to(root).as_posix()
        if name in tracked and name != FILE_DIGESTS:
            raise ValueError(f"{destination}: output must not replace a tracked corpus file")
        # A separate output can be untracked, but can never exclude an input.
        outputs.add(name)
    _check_clean(root, outputs)

    files = {}
    for name in sorted(tracked - {FILE_DIGESTS}):
        path = root / name
        _local_file(root, path)
        _require_materialized(path)
        files[name] = _sha256(path)
    _check_clean(root, outputs)
    if _tracked_entries(root) != entries:
        raise ValueError(f"{root}: tracked files changed while writing the manifest")
    record = {"schema_version": FILE_DIGEST_SCHEMA, "files": files}
    with destination.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(record, sort_keys=True, indent=1) + "\n")
    return destination


def main(argv: list[str] | None = None) -> int:
    parser = _Parser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path, metavar="DIR",
                        help="Clean corpus Git checkout with materialized Git-LFS files.")
    parser.add_argument("--output", type=Path, metavar="FILE",
                        help=f"Output file (default: DIR/{FILE_DIGESTS}).")
    try:
        args = parser.parse_args(argv)
    except ValueError as error:
        print(f"ERROR arguments: {error}")
        return 2
    try:
        destination = write_file_digests(args.corpus, args.output)
    except ValueError as error:
        print(f"REFUSED {error}")
        return 1
    except Exception as error:
        message = " ".join(str(error).splitlines())
        print(f"ERROR {type(error).__name__}: {message}")
        return 2
    print(f"WROTE {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
