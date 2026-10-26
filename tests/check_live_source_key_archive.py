"""Require the strict diagnostic in its control and absent from the built library archive."""

import argparse
from pathlib import Path
import sys


NEEDLE = b"Pine live source key spelling is not represented"


def contains_needle(filename: str) -> bool:
    if not filename:
        raise ValueError("empty scan input path: ''")
    with Path(filename).open("rb") as stream:
        chunk = stream.read(65536)
        if not chunk:
            raise ValueError(f"empty scan input: {filename}")
        tail = b""
        while chunk:
            window = tail + chunk
            if NEEDLE in window:
                return True
            tail = window[-(len(NEEDLE) - 1):]
            chunk = stream.read(65536)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--strict-executable", required=True)
    args = parser.parse_args()
    try:
        strict_present = contains_needle(args.strict_executable)
        archive_present = contains_needle(args.archive)
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    if not strict_present:
        print(f"FAIL: positive control missing needle: {args.strict_executable}",
              file=sys.stderr)
        return 1
    if archive_present:
        print(f"FAIL: strict-only needle found in library archive: {args.archive}",
              file=sys.stderr)
        return 1
    print(f"PASS: needle found in {args.strict_executable}; absent from {args.archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
