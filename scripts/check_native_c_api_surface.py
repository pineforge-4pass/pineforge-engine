#!/usr/bin/env python3
"""Every NativeStrategyHost member has a C spelling or a stated exclusion.

R5 lane N8. The audit's G1-b finding was that fourteen C++ capabilities had no
C spelling and that the header's own exclusion list named three unrelated ones,
so a reader could not tell "not exposed" from "nobody looked".  This guard
removes that possibility mechanically:

  * the public surface of `class NativeStrategyHost`
    (include/pineforge/native_host.hpp) is the authority for WHAT exists;
  * the COVERAGE block in include/pineforge/native_c_api.h is the authority for
    what each member's C spelling is, or why it has none;
  * the two must agree EXACTLY -- no member missing from the block, no row in
    the block naming a member that no longer exists.

A `[C]` row must also name a symbol or field that really is declared in the C
headers, so a spelling cannot be claimed for something that was never added.
Exclusion rows carry their reason in the same line and are free text.

R5 lane E15 adds the members that census cannot see: a protected member of
the base, `class BacktestEngine` (include/pineforge/engine.hpp), that a host
is documented to call or override from its callbacks. Those are opt-in: a
`// @host-seam` line marks each one's declaration, and the block's
BASE-CLASS SEAMS list must name exactly the marked set, under the same row
rules. A marker outside the class, or one that does not precede a member
function, fails too, so a marker cannot quietly mark nothing.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "include" / "pineforge" / "native_host.hpp"
ENGINE = ROOT / "include" / "pineforge" / "engine.hpp"
C_API = ROOT / "include" / "pineforge" / "native_c_api.h"
PUBLIC_C = ROOT / "include" / "pineforge" / "pineforge.h"

# Members that are not part of the host's own surface: the copy/move deletions
# the class spells out, and the friend declaration.
_SKIP = frozenset({"NativeStrategyHost", "operator="})

_ROW = re.compile(r"^\s*\*\s+\[(C|--)\]\s+(\w+)\s+(\S.*?)\s*$")

# The opt-in marker for a base-class seam, and the heading of its list.
_SEAM_MARK = re.compile(r"^\s*//\s*@host-seam\b")
_SEAMS_HEADING = "BASE-CLASS SEAMS"


def host_members(text: str) -> set[str]:
    start = text.index("class NativeStrategyHost : public BacktestEngine {")
    end = text.index("\n};", start)
    body = text[start:end]
    # Only the public section: the class has one, ending at `friend class`.
    body = body.split("friend class", 1)[0]
    names: set[str] = set()
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith(("//", "/*", "*", "#")) or not stripped:
            continue
        # A declaration is `<stuff> name(` at paren depth 0 for this line.
        match = re.search(r"(~?\w+)\s*\(", stripped)
        if not match:
            continue
        name = match.group(1)
        if name in _SKIP or name.startswith("~"):
            continue
        # Skip default-argument and initializer noise inside a body.
        if stripped.startswith(("return", "if", "for", "while", "}")):
            continue
        names.add(name)
    return names


def base_seams(text: str) -> tuple[set[str], list[str]]:
    """The BacktestEngine members engine.hpp marks `@host-seam`, and misuses."""
    start = text.index("class BacktestEngine {")
    end = text.index("\n};", start)
    first = text.count("\n", 0, start)
    last = text.count("\n", 0, end)
    lines = text.splitlines()
    names: set[str] = set()
    errors: list[str] = []
    for number, line in enumerate(lines):
        if not _SEAM_MARK.match(line):
            continue
        if not first < number <= last:
            errors.append(f"engine.hpp:{number + 1}: a @host-seam marker outside class "
                          "BacktestEngine")
            continue
        name = None
        for follow in lines[number + 1:last + 1]:
            stripped = follow.strip()
            if not stripped or stripped.startswith(("//", "/*", "*")):
                continue
            match = re.search(r"(~?\w+)\s*\(", stripped)
            name = match.group(1) if match else None
            break
        if name is None:
            errors.append(f"engine.hpp:{number + 1}: a @host-seam marker is not followed by a "
                          "member function declaration")
            continue
        names.add(name)
    return names, errors


def coverage_bounds(text: str) -> tuple[int, int]:
    try:
        start = text.index("COVERAGE")
        return start, text.index("HARDENING RULES", start)
    except ValueError:  # pragma: no cover - the block is required
        print("check_native_c_api_surface: no COVERAGE block in native_c_api.h",
              file=sys.stderr)
        raise SystemExit(2)


def _rows(block: str, seen: set[str]) -> tuple[dict[str, str], dict[str, str]]:
    spelled: dict[str, str] = {}
    excluded: dict[str, str] = {}
    for line in block.splitlines():
        match = _ROW.match(line)
        if not match:
            continue
        kind, name, detail = match.groups()
        if name in seen:
            print(f"check_native_c_api_surface: {name} listed twice", file=sys.stderr)
            raise SystemExit(1)
        seen.add(name)
        (spelled if kind == "C" else excluded)[name] = detail
    return spelled, excluded


def coverage_rows(text: str) -> tuple[tuple[dict[str, str], dict[str, str]],
                                      tuple[dict[str, str], dict[str, str]]]:
    """(spelled, excluded) of the host rows, then of the BASE-CLASS SEAMS rows."""
    start, end = coverage_bounds(text)
    block = text[start:end]
    split = block.find(_SEAMS_HEADING)
    if split < 0:
        print(f"check_native_c_api_surface: no {_SEAMS_HEADING} list in the COVERAGE block",
              file=sys.stderr)
        raise SystemExit(2)
    seen: set[str] = set()
    return _rows(block[:split], seen), _rows(block[split:], seen)


def main() -> int:
    host_text = HOST.read_text(encoding="utf-8")
    c_api_text = C_API.read_text(encoding="utf-8")
    public_text = PUBLIC_C.read_text(encoding="utf-8")
    engine_text = ENGINE.read_text(encoding="utf-8")

    members = host_members(host_text)
    seams, marker_errors = base_seams(engine_text)
    (spelled, excluded), (seam_spelled, seam_excluded) = coverage_rows(c_api_text)
    listed = set(spelled) | set(excluded)
    seams_listed = set(seam_spelled) | set(seam_excluded)
    # A claimed spelling is resolved against the DECLARATIONS only: the
    # COVERAGE block itself is cut out first, so a row can never be the sole
    # evidence for the symbol it names.
    block_start, block_end = coverage_bounds(c_api_text)
    declarations = c_api_text[:block_start] + c_api_text[block_end:] + public_text

    failures: list[str] = []
    missing = sorted(members - listed)
    if missing:
        failures.append(
            "members with neither a C spelling nor an exclusion: " + ", ".join(missing))
    stale = sorted(listed - members)
    if stale:
        failures.append(
            "COVERAGE rows naming members that no longer exist: " + ", ".join(stale))

    failures.extend(marker_errors)
    missing_seams = sorted(seams - seams_listed)
    if missing_seams:
        failures.append("marked BacktestEngine seams with neither a C spelling nor an "
                        "exclusion: " + ", ".join(missing_seams))
    stale_seams = sorted(seams_listed - seams)
    if stale_seams:
        failures.append(f"{_SEAMS_HEADING} rows naming members engine.hpp does not mark "
                        "@host-seam: " + ", ".join(stale_seams))

    # Every claimed spelling must name something the C headers declare.
    for name, detail in sorted({**spelled, **seam_spelled}.items()):
        tokens = re.findall(r"[A-Za-z_][A-Za-z_0-9]*", detail)
        if not any(token in declarations for token in tokens
                   if token.startswith(("strategy_", "pf_", "PF_"))):
            failures.append(f"{name}: the C spelling '{detail}' names no declared symbol")

    if failures:
        for failure in failures:
            print("check_native_c_api_surface: " + failure, file=sys.stderr)
        return 1

    print(f"check_native_c_api_surface: {len(members)} NativeStrategyHost members, "
          f"{len(spelled)} with a C spelling, {len(excluded)} excluded with a reason; "
          f"{len(seams)} marked BacktestEngine seams, {len(seam_spelled)} with a C spelling, "
          f"{len(seam_excluded)} excluded with a reason")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
