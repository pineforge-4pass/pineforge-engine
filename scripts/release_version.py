#!/usr/bin/env python3
"""The engine release's version arithmetic, for .github/workflows/release.yml.

The workflow is the only writer of VERSION; it calls this script instead of
computing versions in YAML (lane REL10).

``next`` validates VERSION and the requested release, and prints the typed
outputs the later jobs read::

    python3 scripts/release_version.py next --bump patch|minor|major \\
        [--override X.Y.Z[-rc.N]] [--version-file VERSION] \\
        [--tags-file FILE] [--github-output FILE]

    current=0.14.0
    next=1.0.0-rc.1
    tag=v1.0.0-rc.1
    prerelease=true
    previous=v0.13.1

A version is ``X.Y.Z`` or a release candidate ``X.Y.Z-rc.N`` (N >= 1, no
leading zeros anywhere): the one prerelease spelling the release hub pairs
exactly with codegen's. ``--override`` names the next version exactly and wins
over ``--bump``; a current release candidate has no bump, so its successor
(the next candidate, or the final release) is always an override. The next
version must sort above the current one and its tag must not exist yet.
``previous`` is the release-notes base: the highest existing tag below the
next version, or, for a final release, the highest final one, so that a final
release's notes cover its candidates too. Tags are ordered by semver
precedence, where ``X.Y.Z-rc.N`` sorts below ``X.Y.Z``; git's ``v:refname``
sort puts it above.

``check-install`` verifies a staged install before a tarball is packed: the
generated ``pineforge/version.h`` and the CMake package files must carry the
full version (``-rc.N`` included) and its numeric ``X.Y.Z``::

    python3 scripts/release_version.py check-install --prefix DIR --expect X.Y.Z[-rc.N]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable, NamedTuple

_NUMBER = r"(0|[1-9][0-9]*)"
VERSION_RE = re.compile(rf"{_NUMBER}\.{_NUMBER}\.{_NUMBER}(?:-rc\.([1-9][0-9]*))?")


class Version(NamedTuple):
    major: int
    minor: int
    patch: int
    rc: int | None

    @property
    def prerelease(self) -> bool:
        return self.rc is not None

    @property
    def numeric(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    def key(self) -> tuple[int, int, int, int, int]:
        # Semver precedence: X.Y.Z-rc.N < X.Y.Z, and rc.N orders by N.
        final = 1 if self.rc is None else 0
        return (self.major, self.minor, self.patch, final, self.rc or 0)

    def __str__(self) -> str:
        return self.numeric + ("" if self.rc is None else f"-rc.{self.rc}")


class ReleaseError(ValueError):
    """A request the release must refuse; the message names what to pass."""


def parse_version(raw: str) -> Version:
    match = VERSION_RE.fullmatch(raw)
    if not match:
        raise ReleaseError(f"invalid version {raw!r}: expected X.Y.Z or X.Y.Z-rc.N "
                           "(N >= 1, no leading zeros, no 'v')")
    major, minor, patch = (int(match.group(i)) for i in (1, 2, 3))
    rc = int(match.group(4)) if match.group(4) else None
    return Version(major, minor, patch, rc)


def parse_tags(tags: Iterable[str]) -> list[Version]:
    """The ``vX.Y.Z[-rc.N]`` tags among ``tags``; anything else is ignored."""
    found = []
    for tag in tags:
        tag = tag.strip()
        if tag.startswith("v"):
            try:
                found.append(parse_version(tag[1:]))
            except ReleaseError:
                pass
    return found


def next_version(current: str, bump: str, override: str = "") -> Version:
    now = parse_version(current)
    if override:
        new = parse_version(override)
    elif bump not in ("patch", "minor", "major"):
        raise ReleaseError(f"invalid bump {bump!r}: expected patch, minor or major")
    elif now.prerelease:
        raise ReleaseError(
            f"VERSION is the release candidate {now}: pass override, the next "
            f"candidate (for example {now.numeric}-rc.{now.rc + 1}) or the final "
            f"release {now.numeric}")
    elif bump == "patch":
        new = Version(now.major, now.minor, now.patch + 1, None)
    elif bump == "minor":
        new = Version(now.major, now.minor + 1, 0, None)
    else:
        new = Version(now.major + 1, 0, 0, None)
    if new.key() <= now.key():
        raise ReleaseError(f"release {new} does not sort above VERSION {now}")
    return new


def previous_tag(new: Version, tags: Iterable[Version]) -> str:
    """The release-notes base for ``new``: see the module docstring."""
    below = [t for t in tags if t.key() < new.key() and (new.prerelease or not t.prerelease)]
    return f"v{max(below, key=Version.key)}" if below else ""


def plan(current: str, bump: str, override: str, tags: Iterable[str]) -> dict[str, str]:
    new = next_version(current, bump, override)
    known = parse_tags(tags)
    if any(t == new for t in known):
        raise ReleaseError(f"tag v{new} already exists")
    return {
        "current": current,
        "next": str(new),
        "tag": f"v{new}",
        "prerelease": "true" if new.prerelease else "false",
        "previous": previous_tag(new, known),
    }


def _define(text: str, name: str) -> str | None:
    match = re.search(rf"^#define {name}\s+(.+?)\s*$", text, re.MULTILINE)
    return match.group(1) if match else None


def check_install(prefix: Path, expect: str) -> list[str]:
    """What a staged install gets wrong about ``expect``; empty when nothing."""
    version = parse_version(expect)
    problems = []
    header = prefix / "include" / "pineforge" / "version.h"
    config_dir = next(iter(sorted(prefix.glob("lib*/cmake/PineForge"))), None)
    if not header.is_file():
        problems.append(f"{header} is missing")
    else:
        text = header.read_text(encoding="utf-8")
        wanted = {
            "PINEFORGE_VERSION_MAJOR": str(version.major),
            "PINEFORGE_VERSION_MINOR": str(version.minor),
            "PINEFORGE_VERSION_PATCH": str(version.patch),
            "PINEFORGE_VERSION_STRING": f'"{version.numeric}"',
            "PINEFORGE_VERSION_FULL": f'"{version}"',
        }
        for name, value in wanted.items():
            found = _define(text, name)
            if found != value:
                problems.append(f"{header}: {name} is {found!r}, expected {value!r}")
    if config_dir is None:
        problems.append(f"{prefix}/lib*/cmake/PineForge is missing")
    else:
        config = config_dir / "PineForgeConfig.cmake"
        config_version = config_dir / "PineForgeConfigVersion.cmake"
        if not config.is_file():
            problems.append(f"{config} is missing")
        elif f'set(PineForge_VERSION_FULL "{version}")' not in config.read_text(encoding="utf-8"):
            problems.append(f"{config} does not set PineForge_VERSION_FULL to {version}")
        if not config_version.is_file():
            problems.append(f"{config_version} is missing")
        elif f'set(PACKAGE_VERSION "{version.numeric}")' not in config_version.read_text(
                encoding="utf-8"):
            problems.append(f"{config_version} does not set PACKAGE_VERSION to {version.numeric}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    step = commands.add_parser("next", help="compute and validate the next release")
    step.add_argument("--bump", required=True, choices=("patch", "minor", "major"))
    step.add_argument("--override", default="",
                      help="the exact next version, X.Y.Z or X.Y.Z-rc.N; wins over --bump")
    step.add_argument("--version-file", type=Path, default=Path("VERSION"))
    step.add_argument("--tags-file", type=Path,
                      help="`git tag --list 'v*'` output; refuses an existing tag and "
                           "sets previous")
    step.add_argument("--github-output", type=Path, help="append key=value lines here")
    check = commands.add_parser("check-install", help="verify a staged install's version")
    check.add_argument("--prefix", type=Path, required=True)
    check.add_argument("--expect", required=True)
    args = parser.parse_args(argv)

    try:
        if args.command == "check-install":
            problems = check_install(args.prefix, args.expect)
            for problem in problems:
                print(f"release_version: {problem}", file=sys.stderr)
            if problems:
                return 1
            print(f"release_version: {args.prefix} carries {args.expect}")
            return 0
        current = args.version_file.read_text(encoding="utf-8").strip()
        tags = (args.tags_file.read_text(encoding="utf-8").splitlines()
                if args.tags_file else [])
        fields = plan(current, args.bump, args.override, tags)
    except ReleaseError as error:
        print(f"release_version: {error}", file=sys.stderr)
        return 1
    for key, value in fields.items():
        print(f"{key}={value}")
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as output:
            for key, value in fields.items():
                output.write(f"{key}={value}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
