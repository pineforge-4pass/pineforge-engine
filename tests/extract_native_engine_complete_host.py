#!/usr/bin/env python3
"""Build and run the Complete host directly from native-engine.md.

The CTest command compares the generated translation unit with the live page
before executing it, so ctest cannot silently run a binary from an older page.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


def extracted(page: Path) -> str:
    lines = page.read_text(encoding="utf-8").splitlines()
    headings = [i for i, line in enumerate(lines) if line == "## C++ example"]
    if len(headings) != 1:
        raise ValueError(f"expected one C++ example section, found {len(headings)}")
    start = headings[0]
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].startswith("## ")), len(lines))
    intros = [i for i in range(start + 1, end)
              if lines[i].startswith("Complete host:")]
    if len(intros) != 1:
        raise ValueError(f"expected one Complete host introduction, found {len(intros)}")
    fences = [i for i in range(intros[0] + 1, end) if lines[i] == "```cpp"]
    if len(fences) != 1:
        raise ValueError(f"expected one C++ block after Complete host, found {len(fences)}")
    first = fences[0] + 1
    last = next((i for i in range(first, end) if lines[i] == "```"), None)
    if last is None:
        raise ValueError("Complete host block has no closing fence")
    body = "\n".join(lines[first:last]) + "\n"
    if "int main()" not in body or "native_events(0)" not in body:
        raise ValueError("Complete host no longer has its runnable event check")
    return f'#line {first + 1} "docs/pages/native-engine.md"\n' + body


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--page", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--run", type=Path)
    args = parser.parse_args()
    try:
        source = extracted(args.page)
        if args.run is None:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(source, encoding="utf-8")
            return 0
        if not args.out.exists() or args.out.read_text(encoding="utf-8") != source:
            raise ValueError("Complete host page changed after build; rebuild its target")
        return subprocess.run([str(args.run)], timeout=120, check=False).returncode
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"Complete host extraction: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
