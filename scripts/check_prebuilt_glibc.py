#!/usr/bin/env python3
"""Refuse staged prebuilt libraries requiring glibc newer than 2.36.

Internal Linux CI tool: pass exactly one explicit staging directory. Recursively
inspect every .a, .so and .so.*; require a nonempty inventory. Policy is fixed.
GNU ar/readelf must already be installed in the OS default executable search
path. Never extract archives, load libraries, execute payloads, or write files.

--baseline-root DIR adds the released-baseline comparison. DIR is an already
unpacked released archive (the workflow checks its digest and unpacks it; this
tool still extracts nothing). The same strict inspection reads every library
under DIR, and the highest numeric GLIBC_ tag over all staged libraries may not
exceed the highest over all baseline libraries: equal or lower passes, higher
fails. A tree with no numeric tag ranks below any tag. The baseline is read, not
judged: the fixed ceiling and the __isoc23_* rule apply to the staging tree
only, since a released archive may predate them. A missing, empty or partly
uninspectable baseline, or one overlapping the staging root, fails closed. No
expected value is configured anywhere: the verdict is a function of the two
inspected trees.

All symlinks (including directory links), protected/key/config/.env* paths,
unreadable entries, thin archives, non-ELF members, and wrong ELF types fail.
Archives must contain REL objects with inspectable symbol tables; shared objects
must be DYN with dynamic symbols. GNU ar's ordered member list must exactly
match readelf's member blocks, including duplicate names. Index/string-table
archive bookkeeping is not an ELF payload member.

Versions come from ELF symbol suffixes and GNU version sections, never arbitrary
file strings. Numeric grammar is GLIBC_<digits>.<digits>[.<digits>...].
Compare numeric components with zero padding: 2.9 < 2.36 == 2.36.0 < 2.36.1.
GLIBC_PRIVATE and every other nonnumeric GLIBC_* tag fail explicitly; unrelated
namespaces such as GLIBCXX_* are reported separately. Any __isoc23_* symbol,
defined or undefined and with or without a version suffix, fails conservatively.
FILE/SECTION entries describe source names/sections, not linkable references.

JSON lines are an internal evidence log, not a product failure catalog/schema.
Inventory precedes library-content reads. Each child record preserves its
return code and complete stdout/stderr (UTF-8 with reversible surrogateescape).
Any child error, stderr diagnostic, empty required output, incomplete table,
unknown output grammar, or changed inventory/file fails closed. Successful
inspection with no numeric GLIBC tag reports max_glibc=null, not a guessed floor.
Exit 0 requires complete nonempty inspection and no issues; exit 1 is refusal,
and argparse uses exit 2 for invalid invocation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys


CEILING = (2, 36)
PROTECTED = {
    ".git", ".ssh", ".aws", ".codex", ".agents", ".config", ".gnupg",
    ".kube", ".azure", ".netrc", "config", "configs", "credentials",
    "keys", "secrets",
}
SYMBOL_HEADER = re.compile(
    r"^Symbol table '([^']+)' contains ([0-9]+) entr(?:y|ies):$")
SYMBOL_ROW = re.compile(
    r"^\s*([0-9]+):\s+([0-9a-fA-F]+)\s+([0-9]+|0x[0-9a-fA-F]+)"
    r"\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)(?:\s+(.*))?$")
VERSION_HEADER = re.compile(
    r"^Version (symbols|definition|needs) section '([^']+)' "
    r"contains ([0-9]+) entr(?:y|ies):$")
OFFSET = r"(?:0x)?[0-9a-fA-F]+:"


class InspectionError(Exception):
    pass


# Fields added to every record while the released baseline is being read.
EVENT_CONTEXT: dict[str, str] = {}


def emit(event: str, **fields) -> None:
    print(json.dumps({"event": event, **EVENT_CONTEXT, **fields}, ensure_ascii=True),
          flush=True)


def issue(issues: list[dict], path: Path, cause: str, **fields) -> None:
    row = {"path": str(path), "cause": cause, **fields}
    issues.append(row)
    emit("issue", **row)


def protected(name: str) -> bool:
    lower = name.lower()
    return (lower in PROTECTED or lower.startswith(".env")
            or lower.endswith((".key", ".pem"))
            or lower.startswith(("id_rsa", "id_dsa", "id_ecdsa", "id_ed25519")))


def validate_component(name: str) -> None:
    if name in ("", ".", "..") or any(ord(char) < 32 or ord(char) == 127 for char in name):
        raise InspectionError(f"unsafe path component {name!r}")
    if protected(name):
        raise InspectionError(f"protected path component {name!r}")


def library_kind(name: str) -> str | None:
    if name.endswith(".a"):
        return "archive"
    if re.search(r"\.so(?:\..*)?\Z", name):
        return "shared"
    return None


def identity(info: os.stat_result) -> dict:
    return {
        "device": info.st_dev, "inode": info.st_ino, "mode": info.st_mode,
        "size": info.st_size, "mtime_ns": info.st_mtime_ns, "ctime_ns": info.st_ctime_ns,
    }


def ancestor_identity(info: os.stat_result) -> tuple[int, int, int]:
    # Ancestors can gain unrelated entries without being replaced.
    return info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode)


def directory_flags() -> int:
    return os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


def open_root(root: Path) -> int:
    """Open each absolute component without following even an ancestor link."""
    descriptor = os.open(root.anchor, directory_flags())
    try:
        current = Path(root.anchor)
        for name in root.parts[1:]:
            validate_component(name)
            current /= name
            info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            if stat.S_ISLNK(info.st_mode):
                raise InspectionError(f"root symlink refused: {current}")
            if not stat.S_ISDIR(info.st_mode):
                raise InspectionError(f"root component is not a directory: {current}")
            child = os.open(name, directory_flags(), dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
            if ancestor_identity(os.fstat(descriptor)) != ancestor_identity(info):
                raise InspectionError(f"root component changed while opening: {current}")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def open_directory(root_fd: int, relative: Path, directories: dict) -> int:
    descriptor = os.dup(root_fd)
    try:
        current = Path(".")
        if identity(os.fstat(descriptor)) != directories[current]:
            raise InspectionError("staging root metadata changed")
        for name in relative.parts:
            validate_component(name)
            current /= name
            child = os.open(name, directory_flags(), dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
            if current not in directories or identity(os.fstat(descriptor)) != directories[current]:
                raise InspectionError(f"directory changed since inventory: {current}")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def discover(root: Path, root_fd: int, issues: list[dict]) -> tuple[list[dict], dict]:
    """Metadata only; no content reads, links, ignored walk errors, or glob skips."""
    directories = {Path("."): identity(os.fstat(root_fd))}
    pending = [Path(".")]
    targets = []
    while pending:
        relative = pending.pop()
        path = root / relative
        emit("directory", path=str(path))
        descriptor = None
        try:
            descriptor = open_directory(root_fd, relative, directories)
            info = os.fstat(descriptor)
            if not info.st_mode & 0o444 or not info.st_mode & 0o111:
                raise InspectionError("directory is unreadable or unsearchable by mode")
            with os.scandir(descriptor) as iterator:
                entries = sorted(iterator, key=lambda entry: entry.name)
            for entry in entries:
                child_relative = relative / entry.name
                child_path = root / child_relative
                kind = library_kind(entry.name)
                target = None
                if kind:
                    target = {"relative": child_relative, "path": child_path,
                              "kind": kind, "identity": None, "admitted": False}
                    targets.append(target)
                try:
                    validate_component(entry.name)
                    child_info = os.stat(entry.name, dir_fd=descriptor, follow_symlinks=False)
                    if target is not None:
                        target["identity"] = identity(child_info)
                    if stat.S_ISLNK(child_info.st_mode):
                        raise InspectionError("symlink refused (including internal/broken links)")
                    if stat.S_ISDIR(child_info.st_mode):
                        if target is not None:
                            issue(issues, child_path, "library path is not a regular file")
                        directories[child_relative] = identity(child_info)
                        pending.append(child_relative)
                    elif not stat.S_ISREG(child_info.st_mode):
                        raise InspectionError("staging entry is not a regular file or directory")
                    elif not child_info.st_mode & 0o444:
                        raise InspectionError("file is unreadable by mode")
                    elif target is not None:
                        target["admitted"] = True
                except (InspectionError, OSError) as error:
                    issue(issues, child_path, str(error))
        except (InspectionError, OSError) as error:
            issue(issues, path, f"directory inventory failed: {error}")
        finally:
            if descriptor is not None:
                os.close(descriptor)
    targets.sort(key=lambda target: str(target["path"]))
    return targets, directories


def run_tool(argv: list[str], path: Path, descriptor: int | None = None,
             allow_empty: bool = False) -> str:
    try:
        result = subprocess.run(
            argv, capture_output=True, pass_fds=() if descriptor is None else (descriptor,),
            env={"LC_ALL": "C", "LANG": "C", "PATH": os.defpath})
    except OSError as error:
        emit("command", path=str(path), argv=argv, returncode=None,
             stdout=None, stderr=None, error=str(error))
        raise InspectionError(f"tool could not start: {error}") from error
    stdout = result.stdout.decode("utf-8", "surrogateescape")
    stderr = result.stderr.decode("utf-8", "surrogateescape")
    emit("command", path=str(path), argv=argv, returncode=result.returncode,
         stdout=stdout, stderr=stderr)
    if result.returncode != 0:
        raise InspectionError(f"tool {argv[0]} failed with returncode {result.returncode}")
    if stderr:
        raise InspectionError(f"tool {argv[0]} emitted stderr; see preserved command record")
    if not stdout and not allow_empty:
        raise InspectionError(f"tool {argv[0]} returned empty required output")
    return stdout


def installed_tools(root: Path, issues: list[dict]) -> dict[str, str]:
    tools = {}
    for name in ("ar", "readelf"):
        try:
            path = shutil.which(name, path=os.defpath)
            if path is None:
                raise InspectionError(f"installed GNU tool missing: {name}")
            real = Path(os.path.realpath(path))
            if real == root or root in real.parents:
                raise InspectionError(f"tool must be installed outside the payload root: {real}")
            if not stat.S_ISREG(real.stat().st_mode):
                raise InspectionError(f"tool is not a regular installed executable: {real}")
            emit("tool", name=name, path=str(real))
            version = run_tool([str(real), "--version"], root)
            if not version.splitlines()[0].startswith(f"GNU {name} "):
                raise InspectionError(f"unsupported tool version grammar for {name}")
            tools[name] = str(real)
        except (InspectionError, OSError) as error:
            issue(issues, root, str(error))
    return tools


def version_exceeds_ceiling(version: tuple[int, ...]) -> bool:
    width = max(len(version), len(CEILING))
    return version + (0,) * (width - len(version)) > CEILING + (0,) * (width - len(CEILING))


def is_isoc23_reference(symbol: str) -> bool:
    return symbol.split("@", 1)[0].startswith("__isoc23_")


def numeric_version(name: str) -> tuple[int, ...] | None:
    if not re.fullmatch(r"GLIBC_[0-9]+(?:\.[0-9]+)+", name):
        return None
    return tuple(int(part) for part in name[len("GLIBC_"):].split("."))


def maximum_version(names: list[str]) -> str | None:
    numeric = [(name, numeric_version(name)) for name in names]
    numeric = [(name, parts) for name, parts in numeric if parts is not None]
    if not numeric:
        return None
    width = max(len(parts) for _, parts in numeric)
    name, _ = max(numeric, key=lambda item: item[1] + (0,) * (width - len(item[1])))
    return name[len("GLIBC_"):]


def tree_maximum(rows: list[dict]) -> tuple[int, ...] | None:
    """Highest numeric GLIBC_ tag over every inspected library of one tree."""
    highest = maximum_version(sorted({name for row in rows for name in row["versions"]}))
    return None if highest is None else tuple(int(part) for part in highest.split("."))


def version_text(version: tuple[int, ...] | None) -> str | None:
    return None if version is None else ".".join(str(part) for part in version)


def glibc_label(version: tuple[int, ...] | None) -> str:
    return "none" if version is None else "GLIBC_" + version_text(version)


def exceeds_baseline(candidate: tuple[int, ...] | None,
                     baseline: tuple[int, ...] | None) -> bool:
    """True when the staged tree's highest numeric tag is above the baseline's.

    Components compare with zero padding, so 2.36 equals 2.36.0. No numeric tag
    at all ranks below every tag: a staged tag above a tagless baseline exceeds
    it, and a tagless staged tree never does.
    """
    if candidate is None:
        return False
    if baseline is None:
        return True
    width = max(len(candidate), len(baseline))
    return (candidate + (0,) * (width - len(candidate))
            > baseline + (0,) * (width - len(baseline)))


def roots_overlap(left: Path, right: Path) -> bool:
    first, second = (Path(os.path.abspath(path)) for path in (left, right))
    return first == second or first in second.parents or second in first.parents


def version_sections(lines: list[str], dynamic_count: int | None) -> set[str]:
    headers = [(index, VERSION_HEADER.fullmatch(line))
               for index, line in enumerate(lines) if line.startswith("Version ")]
    if any(match is None for _, match in headers):
        raise InspectionError("unparsable version section header")
    absent = lines.count("No version information found in this file.")
    if not headers:
        if absent != 1:
            raise InspectionError("missing/unparsable version inspection result")
        return set()
    if absent:
        raise InspectionError("contradictory version inspection result")
    names = set()
    kinds = set()
    for position, (start, match) in enumerate(headers):
        kind, _, count_text = match.groups()
        if kind in kinds:
            raise InspectionError(f"duplicate version {kind} section")
        kinds.add(kind)
        count = int(count_text)
        end = headers[position + 1][0] if position + 1 < len(headers) else len(lines)
        rows = [line.strip() for line in lines[start + 1:end] if line.strip()]
        if not rows or not re.fullmatch(
                r"Addr:\s+0x[0-9a-fA-F]+\s+Offset:\s+0x[0-9a-fA-F]+"
                r"\s+Link:\s+[0-9]+(?:\s+\([^)]*\))?", rows.pop(0)):
            raise InspectionError(f"unparsable version {kind} address metadata")
        if count == 0:
            raise InspectionError(f"empty version {kind} section")
        if kind == "symbols":
            consumed = 0
            for row in rows:
                address = re.fullmatch(r"([0-9a-fA-F]+):\s+(.*)", row)
                if not address or int(address[1], 16) != consumed:
                    raise InspectionError("unparsable/noncontiguous version symbol row")
                rest = address[2]
                while rest:
                    token = re.match(r"\s*([0-9a-fA-F]+)h?\s*\(([^()]*)\)\s*", rest)
                    if token is None:
                        raise InspectionError(f"unparsable version symbol entry: {rest!r}")
                    name = token[2].strip()
                    if name not in ("*local*", "*global*"):
                        if not name or name.startswith("*"):
                            raise InspectionError(f"unknown version symbol entry: {name!r}")
                        names.add(name)
                    consumed += 1
                    rest = rest[token.end():]
            if consumed != count or (dynamic_count is not None and count != dynamic_count):
                raise InspectionError("version symbol count does not match complete dynamic symbols")
        elif kind == "definition":
            consumed = 0
            expected_parents = 0
            for row in rows:
                definition = re.fullmatch(
                    OFFSET + r"\s+Rev:\s+1\s+Flags:\s+.+?\s+Index:\s+[0-9]+"
                    r"\s+Cnt:\s+([0-9]+)\s+Name:\s+(.+)", row)
                if definition:
                    if expected_parents:
                        raise InspectionError("incomplete version definition parents")
                    consumed += 1
                    expected_parents = int(definition[1]) - 1
                    if expected_parents < 0:
                        raise InspectionError("invalid version definition count")
                    names.add(definition[2])
                else:
                    parent = re.fullmatch(OFFSET + r"\s+Parent [0-9]+:\s+(.+)", row)
                    if parent is None or expected_parents <= 0:
                        raise InspectionError(f"unparsable version definition row: {row!r}")
                    names.add(parent[1])
                    expected_parents -= 1
            if consumed != count or expected_parents:
                raise InspectionError("incomplete version definition section")
        else:
            consumed = 0
            expected_names = 0
            for row in rows:
                dependency = re.fullmatch(
                    OFFSET + r"\s+Version:\s+1\s+File:\s+.+?\s+Cnt:\s+([0-9]+)", row)
                if dependency:
                    if expected_names:
                        raise InspectionError("incomplete version requirement names")
                    consumed += 1
                    expected_names = int(dependency[1])
                    if expected_names <= 0:
                        raise InspectionError("empty version requirement")
                else:
                    requirement = re.fullmatch(
                        OFFSET + r"\s+Name:\s+(.+?)\s+Flags:\s+.+?\s+Version:\s+[0-9]+", row)
                    if requirement is None or expected_names <= 0:
                        raise InspectionError(f"unparsable version requirement row: {row!r}")
                    names.add(requirement[1])
                    expected_names -= 1
            if consumed != count or expected_names:
                raise InspectionError("incomplete version requirement section")
    return names


def parse_elf(lines: list[str], required_type: str) -> dict:
    if lines.count("ELF Header:") != 1:
        raise InspectionError("missing/duplicate ELF header")
    if not any(re.match(r"\s*Magic:\s+7f 45 4c 46(?:\s|$)", line) for line in lines):
        raise InspectionError("unparsable ELF magic metadata")
    types = [match[1] for line in lines
             if (match := re.fullmatch(r"\s*Type:\s+(\S+).*", line))]
    machines = [match[1] for line in lines
                if (match := re.fullmatch(r"\s*Machine:\s+(.+)", line))]
    if types != [required_type]:
        raise InspectionError(f"wrong ELF type: expected {required_type}, observed {types}")
    if len(machines) != 1:
        raise InspectionError("missing/unparsable ELF machine metadata")
    if required_type == "DYN" and any("Executable file" in line for line in lines):
        raise InspectionError("executable ELF is not a shared library")
    tables = {}
    table_symbols = {}
    versions = set()
    references = []
    isoc23 = []
    meaningful = 0
    index = 0
    while index < len(lines):
        line = lines[index]
        header = SYMBOL_HEADER.fullmatch(line)
        if header is None:
            if line.startswith("Symbol table "):
                raise InspectionError("unparsable symbol table header")
            if SYMBOL_ROW.fullmatch(line):
                raise InspectionError("symbol row outside its declared table coverage")
            index += 1
            continue
        table, count_text = header.groups()
        count = int(count_text)
        if table in tables or count == 0:
            raise InspectionError(f"duplicate/empty symbol table {table!r}")
        tables[table] = count
        table_symbols[table] = 0
        index += 1
        if index >= len(lines) or lines[index].split() != [
                "Num:", "Value", "Size", "Type", "Bind", "Vis", "Ndx", "Name"]:
            raise InspectionError(f"unparsable symbol columns in {table!r}")
        index += 1
        for ordinal in range(count):
            if index >= len(lines) or (row := SYMBOL_ROW.fullmatch(lines[index])) is None:
                raise InspectionError(f"incomplete/unparsable symbol row {table}[{ordinal}]")
            if int(row[1]) != ordinal:
                raise InspectionError(f"noncontiguous symbol table {table!r}")
            symbol = re.sub(r"\s+\([0-9]+\)$", "", (row[8] or "").strip())
            index += 1
            if not symbol or row[4] in ("FILE", "SECTION"):
                continue
            meaningful += 1
            table_symbols[table] += 1
            version = None
            if "@" in symbol:
                suffix = re.fullmatch(r"([^@]+)@{1,2}([^@]+)", symbol)
                if suffix is None:
                    raise InspectionError(f"unparsable versioned symbol {symbol!r}")
                version = suffix[2]
                versions.add(version)
            reference = {"symbol": symbol, "table": table, "index": ordinal,
                         "section": row[7]}
            if version and version.startswith("GLIBC_"):
                references.append(reference)
            if is_isoc23_reference(symbol):
                isoc23.append(reference)
    required_table = ".dynsym" if required_type == "DYN" else ".symtab"
    if not table_symbols.get(required_table):
        raise InspectionError(f"no nonempty inspectable {required_table} symbols")
    versions.update(version_sections(lines, tables.get(".dynsym")))
    glibc = sorted(name for name in versions if name.startswith("GLIBC_"))
    return {
        "elf_type": required_type, "machine": machines[0], "symbol_tables": tables,
        "table_symbols": table_symbols,
        "symbols": meaningful, "versions": glibc,
        "other_versions": sorted(versions.difference(glibc)),
        "max_glibc": maximum_version(glibc),
        "glibc_references": references, "isoc23_references": isoc23,
    }


def archive_blocks(output: str, tool_path: str) -> list[tuple[str, list[str]]]:
    blocks = []
    prefix = f"File: {tool_path}("
    for line in output.splitlines():
        if line.startswith("File: "):
            if not line.startswith(prefix) or not line.endswith(")"):
                raise InspectionError(f"unparsable archive member identity: {line!r}")
            name = line[len(prefix):-1]
            blocks.append((name, []))
        elif blocks:
            blocks[-1][1].append(line)
        elif line.strip():
            raise InspectionError(f"unparsable archive preamble: {line!r}")
    if not blocks:
        raise InspectionError("no ELF archive member inspection blocks")
    return blocks


def policy_issues(path: Path, member: dict, issues: list[dict]) -> None:
    context = {"member": member.get("name"), "ordinal": member.get("ordinal")}
    for name in member["versions"]:
        version = numeric_version(name)
        if version is None:
            reason = "private glibc ABI refused" if name == "GLIBC_PRIVATE" else "unknown GLIBC version grammar"
            issue(issues, path, f"{reason}: {name}", **context)
        elif version_exceeds_ceiling(version):
            issue(issues, path, f"{name} exceeds fixed GLIBC_2.36 ceiling", **context)
    for reference in member["isoc23_references"]:
        issue(issues, path, f"forbidden __isoc23_* symbol reference: {reference['symbol']}",
              **context, reference=reference)


def inspect_target(root_fd: int, target: dict, directories: dict,
                   tools: dict[str, str], issues: list[dict], policy: bool = True) -> dict:
    path = target["path"]
    row = {
        "path": str(path), "kind": target["kind"], "identity": target["identity"],
        "sha256": None, "size": None, "inspected": False, "members": [],
        "expected_members": None,
        "symbols": 0, "versions": [], "other_versions": [], "max_glibc": None,
        "glibc_references": [], "isoc23_references": [],
    }
    parent_fd = None
    descriptor = None
    before_issues = len(issues)
    try:
        if not target["admitted"]:
            raise InspectionError("target was refused during inventory")
        if set(tools) != {"ar", "readelf"}:
            raise InspectionError("required installed inspection tool unavailable")
        parent_fd = open_directory(root_fd, target["relative"].parent, directories)
        descriptor = os.open(target["relative"].name,
                             os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK,
                             dir_fd=parent_fd)
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            raise InspectionError("opened library is not a regular file")
        if identity(before) != target["identity"]:
            raise InspectionError("library changed since inventory")
        if not before.st_mode & 0o444:
            raise InspectionError("library is unreadable by mode")
        digest = hashlib.sha256()
        head = b""
        size = 0
        while chunk := os.read(descriptor, 1024 * 1024):
            head = (head + chunk)[:8]
            digest.update(chunk)
            size += len(chunk)
        row.update(sha256=digest.hexdigest(), size=size)
        if size != before.st_size:
            raise InspectionError("short/changed library content read")
        if not size:
            raise InspectionError("empty library")
        os.lseek(descriptor, 0, os.SEEK_SET)
        tool_path = f"/proc/self/fd/{descriptor}"
        expected_members = None
        if target["kind"] == "archive":
            if head == b"!<thin>\n":
                raise InspectionError("thin archive refused: members may refer outside staging")
            if head != b"!<arch>\n":
                raise InspectionError("wrong format: expected a regular ar archive")
            listing = run_tool([tools["ar"], "t", tool_path], path, descriptor, allow_empty=True)
            expected_members = listing.splitlines()
            row["expected_members"] = expected_members
            row["members"] = [
                {"ordinal": index, "name": name, "inspected": False}
                for index, name in enumerate(expected_members, 1)]
            emit("archive_inventory", path=str(path), members=[
                {"ordinal": index, "name": name}
                for index, name in enumerate(expected_members, 1)])
            if not expected_members:
                raise InspectionError("empty archive: no payload members")
            if not listing.endswith("\n"):
                raise InspectionError("unparsable archive member inventory")
            for name in expected_members:
                if (not Path(name).parts or Path(name).is_absolute()
                        or ".." in Path(name).parts):
                    raise InspectionError(f"unsafe archive member name: {name!r}")
                for part in Path(name).parts:
                    validate_component(part)
        elif not head.startswith(b"\x7fELF"):
            raise InspectionError("wrong format: expected an ELF shared object")
        output = run_tool([tools["readelf"], "--wide", "--file-header", "--symbols",
                           "--version-info", "--", tool_path], path, descriptor)
        complete = True
        if expected_members is None:
            blocks = [(None, output.splitlines())]
        else:
            blocks = archive_blocks(output, tool_path)
            observed = [name for name, _ in blocks]
            if observed != expected_members:
                issue(issues, path, "archive member coverage differs between ar and readelf",
                      expected=expected_members, observed=observed)
                complete = False
            row["members"] = []
        for ordinal, (name, lines) in enumerate(blocks, 1):
            member = {"name": name, "ordinal": ordinal, "inspected": False}
            try:
                member.update(parse_elf(lines, "REL" if expected_members is not None else "DYN"))
                member["inspected"] = True
                if policy:
                    policy_issues(path, member, issues)
            except (InspectionError, ValueError) as error:
                issue(issues, path, str(error), member=name, ordinal=ordinal)
                complete = False
            if expected_members is not None:
                row["members"].append(member)
            if member["inspected"]:
                row["symbols"] += member["symbols"]
                for field in ("versions", "other_versions", "glibc_references", "isoc23_references"):
                    if field.endswith("references"):
                        row[field].extend({**reference, "member": name, "ordinal": ordinal}
                                          for reference in member[field])
                    else:
                        row[field] = sorted(set(row[field]).union(member[field]))
        row["max_glibc"] = maximum_version(row["versions"])
        if identity(os.fstat(descriptor)) != identity(before):
            raise InspectionError("library changed during tool inspection")
        after = os.stat(target["relative"].name, dir_fd=parent_fd, follow_symlinks=False)
        if identity(after) != identity(before):
            raise InspectionError("library path changed during tool inspection")
        row["inspected"] = complete and row["symbols"] > 0
    except (InspectionError, OSError, ValueError) as error:
        issue(issues, path, str(error))
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if parent_fd is not None:
            os.close(parent_fd)
    row["violations"] = issues[before_issues:]
    emit("target", **row)
    return row


def inspect_tree(root: Path, issues: list[dict], policy: bool) -> tuple[list[dict], list[dict]]:
    """Inventory and inspect one explicit root; the fixed policy is for staging only."""
    targets = []
    results = []
    root_fd = None
    emit("root", path=str(root), **({"fixed_glibc_ceiling": "2.36"} if policy else {}))
    try:
        if sys.platform != "linux":
            raise InspectionError("Linux descriptor-based ELF inspection is required")
        if ".." in root.parts:
            raise InspectionError("root parent traversal refused")
        root = root.absolute()
        if root == Path(root.anchor):
            raise InspectionError("filesystem root is not a staging directory")
        root_fd = open_root(root)
        targets, directories = discover(root, root_fd, issues)
        emit("inventory", paths=[str(target["path"]) for target in targets])
        if not targets:
            issue(issues, root, "no shipped .so/.so.*/.a libraries in staging inventory")
        tools = installed_tools(root, issues) if targets else {}
        for target in targets:
            results.append(inspect_target(root_fd, target, directories, tools, issues, policy))
        # Re-open the path and re-inventory metadata to catch late additions,
        # removals, renames, or a replacement root; payloads are never re-read.
        final_fd = open_root(root)
        try:
            final_targets, final_directories = discover(root, final_fd, issues)
            snapshot = lambda rows: [
                (str(row["relative"]), row["kind"], row["identity"], row["admitted"])
                for row in rows]
            if snapshot(final_targets) != snapshot(targets) or final_directories != directories:
                issue(issues, root, "staging library/directory inventory changed during inspection")
            emit("inventory_check", paths=[str(target["path"]) for target in final_targets])
        finally:
            os.close(final_fd)
    except (InspectionError, OSError, ValueError) as error:
        issue(issues, root, f"root inspection failed: {error}")
    finally:
        if root_fd is not None:
            os.close(root_fd)
    return targets, results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("staging_root", type=Path, help="explicit installed prebuilt staging root")
    parser.add_argument("--baseline-root", default=None, metavar="DIR",
                        help="explicit unpacked released archive; the staging tree's highest "
                             "numeric GLIBC_ tag may not exceed the highest found there")
    args = parser.parse_args(argv)
    if args.baseline_root is not None and not args.baseline_root:
        parser.error("--baseline-root must name a directory")
    issues = []
    targets, results = inspect_tree(args.staging_root, issues, True)
    inspected = sum(row["inspected"] for row in results)
    complete = bool(targets) and inspected == len(targets)
    baseline_fields = {}
    if args.baseline_root is not None:
        baseline_root = Path(args.baseline_root)
        baseline_targets = []
        baseline_results = []
        if roots_overlap(args.staging_root, baseline_root):
            issue(issues, baseline_root.absolute(),
                  "baseline root must be separate from the staging root")
        else:
            EVENT_CONTEXT["role"] = "baseline"
            try:
                baseline_targets, baseline_results = inspect_tree(baseline_root, issues, False)
            finally:
                EVENT_CONTEXT.clear()
        baseline_inspected = sum(row["inspected"] for row in baseline_results)
        baseline_fields = {"baseline_discovered": len(baseline_targets),
                           "baseline_inspected": baseline_inspected}
        complete = (complete and bool(baseline_targets)
                    and baseline_inspected == len(baseline_targets))
        if complete:
            staged_highest = tree_maximum(results)
            released_highest = tree_maximum(baseline_results)
            above = exceeds_baseline(staged_highest, released_highest)
            emit("baseline_comparison", staged_max_glibc=version_text(staged_highest),
                 baseline_max_glibc=version_text(released_highest), exceeds=above)
            if above:
                issue(issues, args.staging_root.absolute(),
                      f"staged highest {glibc_label(staged_highest)} exceeds released baseline "
                      f"highest {glibc_label(released_highest)}",
                      staged_max_glibc=version_text(staged_highest),
                      baseline_max_glibc=version_text(released_highest),
                      baseline_root=str(baseline_root.absolute()))
    success = complete and not issues
    emit("summary", verdict="PASS" if success else "FAIL", discovered=len(targets),
         inspected=inspected, archive_members=sum(
             sum(member["inspected"] for member in row["members"]) for row in results),
         issues=len(issues), **baseline_fields)
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
