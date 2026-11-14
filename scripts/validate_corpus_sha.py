#!/usr/bin/env python3
"""Validate a corpus checkout without an engine trade baseline.

Exit 0: valid; 1: invalid corpus; 2: validation could not run.
Builds, derived feeds, probe outputs and logs stay under --work.
The original corpus checkout must be clean before and after validation.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Callable, Mapping


sys.dont_write_bytecode = True
from corpus_trades_identity import ALLOWED_EXTRA_COLUMNS  # noqa: E402


ENGINE = Path(__file__).resolve().parents[1]
FEED = "data/ohlcv_ETH-USDT-USDT_1m.csv"
FILE_DIGESTS = "file_digests.json"
FILE_DIGEST_SCHEMA = "pineforge-corpus-file-digests-v1"
ARTIFACT_FILES = {
    "engine_trade_tape_sha256": "engine_trades.csv",
    "generated_cpp_sha256": "generated.cpp",
    "inputs_sha256": "inputs.json",
    "readme_sha256": "README.md",
    "source_sha256": "strategy.pine",
    "tradingview_trade_export_sha256": "tv_trades.csv",
}
DIGEST = re.compile(r"[0-9a-f]{64}")
PROBE = re.compile(r"validation/[A-Za-z0-9][A-Za-z0-9._-]*")
LFS_VERSION = b"version https://git-lfs.github.com/spec/v1"
FILE_PATH = re.compile(r"[A-Za-z0-9._/-]+")
SHARD = re.compile(r"([1-9][0-9]*)/([1-9][0-9]*)")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _local_file(root: Path, path: Path) -> None:
    if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("file must stay inside the corpus and must not be a symlink")
    if not path.is_file():
        raise ValueError("file is missing or is not a regular file")


def _normalized_file_path(name: str) -> bool:
    return bool(FILE_PATH.fullmatch(name)) and all(
        part not in ("", ".", "..") for part in name.split("/"))


def _require_materialized(path: Path) -> None:
    """Refuse pointer bytes rather than hashing them as corpus content."""
    with path.open("rb") as stream:
        if stream.read(len(LFS_VERSION)) == LFS_VERSION:
            raise ValueError("Git-LFS content is not materialized")


def _unique_object(pairs: list[tuple]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _record(root: Path, filename: str, findings: list[str]) -> dict:
    path = root / filename
    try:
        _local_file(root, path)
        result = json.loads(path.read_text(encoding="utf-8"),
                            object_pairs_hook=_unique_object)
        if not isinstance(result, dict):
            raise ValueError("record must be a JSON object")
        return result
    except (OSError, ValueError) as error:
        findings.append(f"records {path}: {error}")
        return {}


def _digest_problem(root: Path, path: Path, expected: object,
                    counts: dict[str, int] | None = None) -> str | None:
    try:
        _local_file(root, path)
        if not isinstance(expected, str) or not DIGEST.fullmatch(expected):
            raise ValueError("recorded sha256 is missing or malformed")
        _require_materialized(path)
        actual = _sha256(path)
        if counts is not None:
            counts["bound"] += 1
        if actual != expected:
            return f"records {path}: sha256 {actual} differs from recorded {expected}"
    except (OSError, ValueError) as error:
        return f"records {path}: {error}"
    return None


def _tracked_entries(root: Path) -> list[tuple[str, str, str]]:
    """Read path, mode and index stage from Git, independently of records."""
    entries = []
    for row in _git(root, "ls-files", "-z", "--stage").split(b"\0"):
        if not row:
            continue
        metadata, filename = row.split(b"\t", 1)
        mode, _, stage = metadata.split()
        entries.append((os.fsdecode(filename), mode.decode("ascii"), stage.decode("ascii")))
    return entries


def _mode_problem(mode: str) -> str | None:
    if mode == "100644":
        return None
    kind = {"120000": "symlink", "100755": "executable", "160000": "gitlink"}.get(mode, "file")
    return f"tracked {kind} with mode {mode} is forbidden; expected mode 100644"


def _census_difference(root: Path, paths: set[str], kind: str,
                       findings: list[str]) -> None:
    ordered = sorted(paths)
    for name in ordered[:50]:
        if _normalized_file_path(name):
            findings.append(f"records {root / name}: {kind} in {FILE_DIGESTS}")
        else:
            findings.append(f"records {root / FILE_DIGESTS}: {kind} path {name!r}")
    if len(ordered) > 50:
        findings.append(f"records {root / FILE_DIGESTS}: {kind}: and {len(ordered) - 50} more")


def _file_census(root: Path, findings: list[str], counts: dict[str, int]) -> dict[str, str]:
    """Compare every tracked file with the manifest's recorded bytes.

    The manifest records what the corpus held when it was written. It detects
    changes without a rewrite and makes deliberate changes visible in its diff.
    It is not a signature; its own bytes are bound by the checked commit.
    """
    record = _record(root, FILE_DIGESTS, findings)
    record_path = root / FILE_DIGESTS
    if record.get("schema_version") != FILE_DIGEST_SCHEMA:
        findings.append(f"records {record_path}: unsupported schema_version")
    files = record.get("files")
    if not isinstance(files, dict) or not files:
        findings.append(f"records {record_path}: files must be a nonempty object")
        files = {}

    entries = _tracked_entries(root)
    tracked = {name for name, _, _ in entries}
    counts["tracked"] = len(tracked)
    if FILE_DIGESTS not in tracked:
        findings.append(f"records {record_path}: manifest must be tracked in Git")
    for name, mode, stage in sorted(entries):
        problem = _mode_problem(mode)
        if problem:
            findings.append(f"records {root / name}: {problem}")
        if stage != "0":
            findings.append(f"records {root / name}: unresolved index stage {stage}")
        if not _normalized_file_path(name):
            findings.append(f"records {record_path}: unusable tracked path {name!r}")
    _census_difference(root, tracked - {FILE_DIGESTS} - set(files),
                       "tracked but unlisted", findings)
    _census_difference(root, set(files) - (tracked - {FILE_DIGESTS}),
                       "listed but untracked", findings)

    digests = {}
    for name, expected in sorted(files.items()):
        if not _normalized_file_path(name) or name == FILE_DIGESTS:
            findings.append(f"records {record_path}: unusable file path {name!r}")
            continue
        problem = _digest_problem(root, root / name, expected, counts)
        if problem:
            findings.append(problem)
        if isinstance(expected, str) and DIGEST.fullmatch(expected):
            digests[name] = expected
    return digests


def _cross_check_digest(root: Path, digests: dict[str, str], name: str,
                        expected: object, source: str, field: str,
                        findings: list[str]) -> None:
    if name not in digests or expected != digests[name]:
        findings.append(
            f"records {root / source}: {field} digest {expected!r} disagrees with "
            f"{root / FILE_DIGESTS}: files[{name!r}]={digests.get(name)!r}")


def _records(root: Path) -> tuple[list[str], dict[str, int], set[str]]:
    findings: list[str] = []
    counts = dict(tapes=0, owned_entries=0, owned_hashes=0, feed=0, tracked=0, bound=0)
    digests = _file_census(root, findings, counts)
    manifest = _record(root, "run_manifest.json", findings)
    owned = _record(root, "owned_strategies.json", findings)
    manifest_path = root / "run_manifest.json"
    owned_path = root / "owned_strategies.json"
    if manifest.get("kind") != "pineforge-corpus-run-manifest":
        findings.append(f"records {manifest_path}: unsupported kind")
    if manifest.get("schemaVersion") != 2:
        findings.append(f"records {manifest_path}: unsupported schemaVersion")
    if owned.get("schema_version") != "pineforge-owned-corpus-manifest-v1":
        findings.append(f"records {owned_path}: unsupported schema_version")

    tapes = manifest.get("tapes")
    if not isinstance(tapes, dict) or not tapes:
        findings.append(f"records {manifest_path}: tapes must be a nonempty map")
        tapes = {}
    counts["tapes"] = len(tapes)
    probes: set[str] = set()
    validation = root / "validation"
    if validation.is_symlink() or not validation.is_dir():
        findings.append(f"records {validation}: expected a directory, not a symlink")
    else:
        for path in sorted(validation.iterdir()):
            if path.is_symlink():
                findings.append(f"records {path}: probe must not be a symlink")
            elif path.is_dir():
                relative = path.relative_to(root).as_posix()
                probes.add(relative)
                if not PROBE.fullmatch(relative):
                    findings.append(f"records {path}: unusable probe directory name")
    if not probes:
        findings.append(f"records {validation}: no probe directories")
    for name in sorted(probes - set(tapes)):
        findings.append(f"records {root / name}: directory is unrecorded in run_manifest.json")
    for name, expected in tapes.items():
        if not PROBE.fullmatch(name):
            findings.append(f"records {manifest_path}: unusable tape path {name!r}")
            continue
        if name not in probes:
            findings.append(f"records {root / name}: recorded probe directory is missing")
        problem = _digest_problem(root, root / name / "engine_trades.csv", expected)
        if problem:
            findings.append(problem)
        _cross_check_digest(root, digests, f"{name}/engine_trades.csv", expected,
                            "run_manifest.json", f"tapes[{name!r}]", findings)
    problem = _digest_problem(root, root / FEED, manifest.get("feedSha256"))
    if problem:
        findings.append(problem)
    else:
        counts["feed"] = 1
    _cross_check_digest(root, digests, FEED, manifest.get("feedSha256"),
                        "run_manifest.json", "feedSha256", findings)

    entries = owned.get("entries")
    if not isinstance(entries, list) or not entries:
        findings.append(f"records {owned_path}: entries must be a nonempty list")
        entries = []
    counts["owned_entries"] = len(entries)
    seen = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            findings.append(f"records {owned_path}: entry {index} must be an object")
            continue
        corpus_id = entry.get("corpus_id")
        if not isinstance(corpus_id, str) or not PROBE.fullmatch(f"validation/{corpus_id}"):
            findings.append(f"records {owned_path}: entry {index} has an unusable corpus_id")
            continue
        name = f"validation/{corpus_id}"
        if name in seen:
            findings.append(f"records {owned_path}: duplicate corpus_id {corpus_id!r}")
        seen.add(name)
        if name not in probes or name not in tapes:
            findings.append(f"records {root / name}: owned probe has no recorded directory")
        artifacts = entry.get("artifacts")
        if not isinstance(artifacts, dict):
            findings.append(f"records {owned_path}: {name} artifacts must be an object")
            artifacts = {}
        if set(artifacts) != set(ARTIFACT_FILES):
            findings.append(f"records {owned_path}: {name} must record exactly the six artifacts")
        for key, filename in ARTIFACT_FILES.items():
            if key in artifacts:
                counts["owned_hashes"] += 1
            problem = _digest_problem(root, root / name / filename, artifacts.get(key))
            if problem:
                findings.append(problem)
            _cross_check_digest(root, digests, f"{name}/{filename}", artifacts.get(key),
                                "owned_strategies.json", f"{name} artifacts[{key!r}]", findings)
    return findings, counts, probes


def check_records(root: Path) -> list[str]:
    """Bind all tracked corpus files and each existing record to checkout bytes."""
    return _records(Path(root))[0]


def lfs_problem(path: Path, pointer: bytes) -> str | None:
    """Compare a committed pointer's oid and size with the hydrated file."""
    try:
        lines = pointer.splitlines()
        if not lines or lines[0] != LFS_VERSION:
            raise ValueError("malformed Git-LFS pointer version")
        oids = [line[len(b"oid sha256:"):] for line in lines
                if line.startswith(b"oid sha256:")]
        sizes = [line[len(b"size "):] for line in lines if line.startswith(b"size ")]
        if len(oids) != 1 or not re.fullmatch(rb"[0-9a-f]{64}", oids[0]):
            raise ValueError("malformed Git-LFS pointer oid")
        if len(sizes) != 1 or not re.fullmatch(rb"[0-9]+", sizes[0]):
            raise ValueError("malformed Git-LFS pointer size")
        _require_materialized(path)
        expected = oids[0].decode("ascii")
        actual = _sha256(path)
        if actual != expected:
            raise ValueError(f"Git-LFS oid {expected} differs from materialized sha256 {actual}")
        if path.stat().st_size != int(sizes[0]):
            raise ValueError("materialized size differs from the Git-LFS pointer size")
    except (OSError, ValueError) as error:
        return f"{path}: {error}"
    return None


def _git(root: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", "--no-optional-locks", "-C", str(root), *arguments],
        check=True, capture_output=True,
    )
    return result.stdout


def _engine_commit(engine: Path) -> str:
    try:
        top = Path(os.fsdecode(_git(
            engine, "rev-parse", "--show-toplevel")).strip()).resolve()
        if top != engine:
            return "unknown"
        revision = _git(engine, "rev-parse", "--verify", "HEAD^{commit}").decode("ascii").strip()
        return revision if re.fullmatch(r"[0-9a-f]{40}", revision) else "unknown"
    except Exception:
        return "unknown"


def _lfs(root: Path) -> tuple[list[str], int]:
    findings = []
    count = 0
    feed_pointer = False
    listing = _git(root, "ls-tree", "-r", "-z", "HEAD", "--", "data")
    for row in listing.split(b"\0"):
        if not row:
            continue
        metadata, filename = row.split(b"\t", 1)
        mode, kind, blob = metadata.split()
        path = root / os.fsdecode(filename)
        if mode != b"100644" or kind != b"blob":
            findings.append(f"lfs {path}: data must contain regular, non-executable files")
            continue
        content = _git(root, "cat-file", "-p", blob.decode("ascii"))
        if not content.startswith(b"version") or b"git-lfs" not in content.splitlines()[0]:
            continue
        count += 1
        if os.fsdecode(filename) == FEED:
            feed_pointer = True
        try:
            _local_file(root, path)
            problem = lfs_problem(path, content)
        except ValueError as error:
            problem = f"{path}: {error}"
        if problem:
            findings.append(f"lfs {problem}")
    if not feed_pointer:
        findings.append(f"lfs {root / FEED}: committed feed is not a Git-LFS pointer")
    return findings, count


def read_subset(path: Path, probes: set[str]) -> list[str]:
    """Read a nonempty, unique list of known, corpus-relative probes."""
    selected = []
    seen = set()
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        name = line.split("#", 1)[0].strip()
        if not name:
            continue
        if not PROBE.fullmatch(name) or name not in probes:
            raise ValueError(f"{path}:{number}: unknown or unusable probe {name!r}")
        if name in seen:
            raise ValueError(f"{path}:{number}: duplicate probe {name!r}")
        selected.append(name)
        seen.add(name)
    if not selected:
        raise ValueError(f"{path}: subset names no probes")
    return selected


def parse_shard(value: str) -> tuple[int, int]:
    match = SHARD.fullmatch(value)
    if match is None:
        raise ValueError("--shard must be I/N with no leading zeros")
    index, count = (int(part) for part in match.groups())
    if index > count:
        raise ValueError("--shard index must be no greater than its count")
    return index, count


def join_shards(logs: Mapping[str, str], shards: int, corpus_commit: str,
                engine_commit: str) -> tuple[list[str], dict[str, int | None]]:
    """Return findings and the probes/selected totals, None when absent or inconsistent."""
    forms = {
        "CORPUS": re.compile(r"CORPUS ([0-9a-f]{40})"),
        "ENGINE": re.compile(r"ENGINE ([0-9a-f]{40}|unknown)"),
        "SHARD": re.compile(r"SHARD ([0-9]+)/([0-9]+) probes=([0-9]+)/([0-9]+) "
                            r"selected=([0-9]+)/([0-9]+)"),
        "RESULT": re.compile(r"RESULT (valid|invalid|error)"),
    }
    findings: list[str] = []
    totals: dict[str, int | None] = {"probes": None, "selected": None}
    owners: dict[int, list[str]] = {}
    counts: dict[str, list[tuple[str, int, int]]] = {"probes": [], "selected": []}
    for name, text in sorted(logs.items()):
        lines = text.splitlines()
        fields = {}
        for kind, form in forms.items():
            candidates = [line for line in lines if line == kind or line.startswith(kind + " ")]
            if len(candidates) != 1:
                findings.append(f"join {name}: unreadable log: expected exactly one "
                                f"{kind} line, found {len(candidates)}")
                continue
            match = form.fullmatch(candidates[0])
            if match is None:
                findings.append(f"join {name}: unreadable log: malformed {kind} line")
            else:
                fields[kind] = match
        if len(fields) != len(forms):
            continue
        if lines[-1] != fields["RESULT"][0]:
            findings.append(f"join {name}: unreadable log: RESULT must be the last line")
        try:
            index, count, size, all_probes, selected, all_selected = (
                int(number) for number in fields["SHARD"].groups())
        except ValueError:
            findings.append(f"join {name}: unreadable log: malformed SHARD numbers")
            continue
        owners.setdefault(index, []).append(name)
        if count != shards:
            findings.append(f"join {name}: shard count {count} differs from --shards {shards}")
        if not 1 <= index <= count:
            findings.append(f"join {name}: shard index {index} is outside 1..{count}")
        result = fields["RESULT"][1]
        if result != "valid":
            findings.append(f"join {name}: shard {index}/{count} RESULT is {result}, expected valid")
        for kind, expected in (("CORPUS", corpus_commit), ("ENGINE", engine_commit)):
            actual = fields[kind][1]
            if actual != expected:
                findings.append(f"join {name}: {kind} {actual} differs from expected {expected}")
        if size < 1:
            findings.append(f"join {name}: shard {index}/{count} slice size must be at least 1, "
                            f"found {size}")
        counts["probes"].append((name, size, all_probes))
        counts["selected"].append((name, selected, all_selected))

    for index in range(1, shards + 1):
        if index not in owners:
            findings.append(f"join: missing shard {index}/{shards}")
    for index, names in sorted(owners.items()):
        if len(names) > 1:
            findings.append(f"join: shard {index}/{shards} appears in multiple logs: "
                            + ", ".join(names))
    for kind, description in (("probes", "slice probe counts"),
                              ("selected", "selected slice counts")):
        entries = counts[kind]
        if not entries:
            continue
        first_name, _, expected = entries[0]
        different = [(name, total) for name, _, total in entries if total != expected]
        if different:
            for name, total in different:
                findings.append(f"join {name}: all {kind} {total} differs from "
                                f"{expected} in {first_name}")
        else:
            totals[kind] = expected
            actual = sum(size for _, size, _ in entries)
            if actual != expected:
                findings.append(f"join shards 1..{shards}: {description} add up to {actual}, "
                                f"expected {expected}")
    return findings, totals


def _join_main(args: argparse.Namespace) -> int:
    findings: list[str] = []
    totals: dict[str, int | None] = {"probes": None, "selected": None}
    logs: dict[str, str] = {}
    corpus = None
    corpus_commit = None
    engine_commit = "unknown"
    shards = None
    error = False
    try:
        corpus, engine = args.corpus.resolve(), args.engine.resolve()
        engine_commit = _engine_commit(engine)
        if args.join is None or args.shards is None:
            raise ValueError("--join and --shards are required together")
        if not re.fullmatch(r"[1-9][0-9]*", args.shards):
            raise ValueError("--shards must be a positive integer without leading zeros, "
                             f"not {args.shards!r}")
        try:
            shards = int(args.shards)
        except ValueError as problem:
            raise ValueError("--shards must be a positive integer without leading zeros, "
                             f"not {args.shards!r}") from problem
        for option, present in (("--shard", args.shard is not None),
                                ("--records-only", args.records_only),
                                ("--work", args.work is not None),
                                ("--subset", args.subset is not None)):
            if present:
                raise ValueError(f"--join cannot be used with {option}")
        if not 1 <= args.jobs <= 8:
            raise ValueError("--jobs must be between 1 and 8")
        top = Path(os.fsdecode(_git(corpus, "rev-parse", "--show-toplevel")).strip()).resolve()
        if top != corpus:
            raise ValueError(f"{corpus}: --corpus must name the Git checkout root")
        corpus_commit = os.fsdecode(_git(corpus, "rev-parse", "HEAD")).strip()
        if engine_commit == "unknown":
            raise ValueError("--join requires a known engine commit for --engine")
        try:
            with os.scandir(args.join) as entries:
                paths = sorted(Path(entry.path) for entry in entries
                               if entry.name.endswith(".log")
                               and entry.is_file(follow_symlinks=False))
            for path in paths:
                try:
                    logs[path.name] = path.read_text(encoding="utf-8")
                except UnicodeError as problem:
                    raise ValueError(f"{path.name}: log is not UTF-8: {problem}") from problem
        except (OSError, ValueError) as problem:
            raise ValueError(f"--join cannot read {args.join}: {problem}") from problem
        findings, totals = join_shards(logs, shards, corpus_commit, engine_commit)
    except Exception as problem:
        error = True
        message = " ".join(str(problem).splitlines())
        findings.append(f"setup {corpus if corpus is not None else 'arguments'}: "
                        f"{type(problem).__name__}: {message}")
    if corpus_commit is not None:
        print(f"CORPUS {corpus_commit}")
    print(f"ENGINE {engine_commit}")
    print(f"JOIN shards={len(logs)}/{shards if shards is not None else '-'} "
          f"probes={totals['probes'] if totals['probes'] is not None else '-'} "
          f"selected={totals['selected'] if totals['selected'] is not None else '-'}")
    for finding in findings:
        print(f"FINDING {finding}")
    code = 2 if error else 1 if findings else 0
    print(f"RESULT {('valid', 'invalid', 'error')[code]}")
    return code


def shard_slice(names: list[str] | set[str], index: int, count: int) -> list[str]:
    if count < 1 or not 1 <= index <= count:
        raise ValueError("shard index and count are out of range")
    return sorted(names)[index - 1::count]


def strategy_target(name: str) -> str:
    """Mirror corpus/CMakeLists.txt's target name for one probe directory."""
    return "strategy_" + re.sub(r"[^A-Za-z0-9_]", "_", name)


def _strategy_targets(names: list[str] | set[str]) -> dict[str, str]:
    targets = {}
    owners = {}
    for name in sorted(names):
        target = strategy_target(name)
        previous = owners.get(target)
        if previous is not None and previous != name:
            raise ValueError(
                f"probe target collision: {previous!r} and {name!r} both map to {target!r}")
        owners[target] = name
        targets[name] = target
    return targets


def _copy_engine(engine: Path, destination: Path) -> None:
    """Copy tracked worktree bytes, excluding submodules and build products."""
    destination.mkdir()
    for row in _git(engine, "ls-files", "--stage", "-z").split(b"\0"):
        if not row:
            continue
        metadata, filename = row.split(b"\t", 1)
        mode, _, stage = metadata.split()
        relative = PurePosixPath(os.fsdecode(filename))
        if stage != b"0":
            raise ValueError(f"{engine / relative}: unresolved index entry")
        if mode == b"160000" or relative.parts[0] == "corpus":
            continue
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"{engine}: unusable tracked path {relative}")
        source, target = engine / relative, destination / relative
        if not source.resolve().is_relative_to(engine):
            raise ValueError(f"{source}: tracked file escapes the engine checkout")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def _copy_ignore(directory: str, names: list[str]) -> set[str]:
    return {name for name in names if name in (".git", "__pycache__")
            or name == "derived" and Path(directory).name == "data"
            or name.endswith((".so", ".dylib", ".dll", ".pyc"))}


def _command(arguments: list[str], cwd: Path, log: Path, environment: dict) -> int:
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(arguments, cwd=cwd, env=environment,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(encoding="utf-8", errors="replace"), end="")
    return result.returncode


def _time_line(stage: str, started: float) -> None:
    print(f"TIME {stage} {time.monotonic() - started:.6f}s", flush=True)


def _header_problem(tape: Path, produced: Path) -> str | None:
    try:
        with tape.open(newline="", encoding="utf-8-sig") as stream:
            expected = next(csv.reader(stream), [])
        with produced.open(newline="", encoding="utf-8-sig") as stream:
            actual = next(csv.reader(stream), [])
        if not expected or not actual:
            return "empty trades file or header"
        if actual[:len(expected)] != expected:
            return f"header changed: tape={expected!r}, produced={actual!r}"
        extras = actual[len(expected):]
        if (any(column not in ALLOWED_EXTRA_COLUMNS for column in extras)
                or len(extras) != len(set(extras))):
            return f"undeclared or repeated trailing columns: {extras!r}"
    except (OSError, ValueError, csv.Error) as error:
        return str(error)
    return None


def collect_probe_results(work: Callable[[str], str | None], items: list[str],
                          jobs: int) -> tuple[list[str], bool]:
    """Collect every probe result even when a worker raises an exception."""
    findings = []
    error = False
    with ThreadPoolExecutor(max_workers=jobs) as workers:
        pending = [(item, workers.submit(work, item)) for item in items]
        for item, future in pending:
            try:
                problem = future.result()
                if problem:
                    findings.append(problem)
            except Exception as problem:
                error = True
                message = " ".join(str(problem).splitlines())
                findings.append(f"run {item}: {type(problem).__name__}: {message}")
    return findings, error


def _build_and_run(corpus: Path, engine: Path, work: Path, probes: set[str],
                   selected: list[str], jobs: int = 8,
                   shard: tuple[int, int] | None = None,
                   started: float | None = None) -> tuple[list[str], bool]:
    if shard is not None and not probes:
        raise ValueError("sharded build has no probes")
    started = time.monotonic() if started is None else started
    targets = _strategy_targets(probes)
    run = Path(tempfile.mkdtemp(prefix="corpus-validation-", dir=work))
    source, build = run / "engine", run / "build"
    print(f"WORK {run}")
    try:
        _copy_engine(engine, source)
        staged = source / "corpus"
        shutil.copytree(corpus, staged, ignore=_copy_ignore, symlinks=True)
    finally:
        _time_line("copy", started)
    environment = os.environ.copy()
    temporary = run / "tmp"
    temporary.mkdir()
    environment.update(PYTHONDONTWRITEBYTECODE="1", TMPDIR=str(temporary),
                       PINEFORGE_REPORT_SINK="0", OPENBLAS_NUM_THREADS="1",
                       OMP_NUM_THREADS="1")
    configure = [
        "cmake", "-S", str(source), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=Release", "-DPINEFORGE_BUILD_TESTS=OFF",
        "-DPINEFORGE_BUILD_TUTORIAL=OFF", "-DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON",
    ]
    try:
        configure_result = _command(
            configure, source, run / "configure.log", environment)
    finally:
        _time_line("configure", started)
    if configure_result:
        return [f"build {engine / 'CMakeLists.txt'}: configure failed; see "
                f"{run / 'configure.log'}"], True
    build_targets = ["corpus_strategies"] if shard is None else [
        targets[name] for name in sorted(probes)]
    build_arguments = [
        "cmake", "--build", str(build), "--target", *build_targets,
        "--parallel", str(jobs),
    ]
    try:
        build_result = _command(
            build_arguments, source, run / "build.log", environment)
    finally:
        _time_line("build", started)
    if build_result:
        target_label = " ".join(build_targets)
        return [f"build {corpus / 'validation'}: {target_label} failed; "
                f"see {run / 'build.log'}"], False
    libraries = {}
    findings = []
    for name in sorted(probes):
        found = [filename for filename in ("strategy.so", "strategy.dylib", "strategy.dll")
                 if (staged / name / filename).is_file()]
        if len(found) != 1:
            findings.append(f"build {corpus / name / 'generated.cpp'}: expected one freshly built strategy library")
        else:
            libraries[name] = found[0]
    if findings:
        _time_line("probes", started)
        return findings, False
    derived = staged / "data" / "derived"
    try:
        derive_result = _command(
            [sys.executable, "-B", str(source / "scripts/derive_corpus_feeds.py"),
             "--corpus-root", str(staged), "--derived-workspace", str(derived)],
            source, run / "derive.log", environment)
    finally:
        _time_line("derive", started)
    if derive_result:
        return [f"feed {corpus / FEED}: derivation failed; see "
                f"{run / 'derive.log'}"], True

    def run_probe(name: str) -> str | None:
        output = run / "outputs" / name / "engine_trades.csv"
        log = run / f"{Path(name).name}.log"
        arguments = [
            sys.executable, "-B", str(source / "scripts/run_strategy.py"),
            str(staged / name), "--corpus-root", str(staged),
            "--derived-workspace", str(derived), "--so-name", libraries[name],
            "--output", str(output),
        ]
        if _command(arguments, source, log, environment):
            return f"run {corpus / name / 'generated.cpp'}: probe failed; see {log}"
        problem = _header_problem(corpus / name / "engine_trades.csv", output)
        if problem:
            return f"header {output}: {problem}"
        return None

    try:
        return collect_probe_results(run_probe, selected, jobs)
    finally:
        _time_line("probes", started)


class _Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise ValueError(message)


def main(argv: list[str] | None = None) -> int:
    parser = _Parser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path, metavar="DIR",
                        help="Corpus Git checkout with materialized Git-LFS data (required).")
    parser.add_argument("--engine", type=Path, default=ENGINE, metavar="DIR",
                        help="Engine checkout (default: repository containing this script).")
    parser.add_argument("--work", type=Path, metavar="DIR",
                        help="Output directory outside both trees (required unless --records-only).")
    parser.add_argument("--records-only", action="store_true",
                        help="Check LFS pointers, recorded digests and cleanliness; build nothing.")
    parser.add_argument("--jobs", type=int, default=8, metavar="N",
                        help="Parallel build and probe workers, from 1 to 8 (default: 8).")
    parser.add_argument("--subset", type=Path, metavar="FILE",
                        help="Probe list (default: engine scripts/corpus_parity_subset.txt).")
    parser.add_argument("--shard", metavar="I/N",
                        help="Build and validate one 1-based slice of the corpus.")
    parser.add_argument("--join", type=Path, metavar="DIR",
                        help="Join saved shard .log files without building; requires --shards.")
    parser.add_argument("--shards", metavar="N",
                        help="Expected shard count for --join, a positive integer "
                             "without leading zeros.")
    joining = any(argument.split("=", 1)[0] in ("--join", "--shards")
                  for argument in (sys.argv[1:] if argv is None else argv))
    counts = dict(tapes=0, owned_entries=0, owned_hashes=0, lfs_pointers=0,
                  feed=0, tracked=0, bound=0)
    findings: list[str] = []
    corpus = None
    status_ready = False
    clean = False
    error = False
    engine_commit = "unknown"
    build_started = None
    try:
        args = parser.parse_args(argv)
        if args.join is not None or args.shards is not None:
            return _join_main(args)
        if args.shard is not None and args.records_only:
            raise ValueError("--records-only cannot be combined with --shard")
        shard = parse_shard(args.shard) if args.shard is not None else None
        corpus, engine = args.corpus.resolve(), args.engine.resolve()
        engine_commit = _engine_commit(engine)
        if not 1 <= args.jobs <= 8:
            raise ValueError("--jobs must be between 1 and 8")
        if not args.records_only and args.work is None:
            raise ValueError("--work is required unless --records-only")
        work = args.work.resolve() if args.work is not None else None
        if work is not None and any(work.is_relative_to(tree) for tree in (corpus, engine)):
            raise ValueError("--work must be outside the corpus and engine trees")
        top = Path(os.fsdecode(_git(corpus, "rev-parse", "--show-toplevel")).strip()).resolve()
        if top != corpus:
            raise ValueError(f"{corpus}: --corpus must name the Git checkout root")
        status_ready = True
        before = _git(corpus, "status", "--porcelain", "--untracked-files=all")
        if before:
            error = True
            for line in os.fsdecode(before).splitlines():
                findings.append(f"clean {corpus}: checkout is dirty before validation: {line}")
        print(f"CORPUS {os.fsdecode(_git(corpus, 'rev-parse', 'HEAD')).strip()}")
        if not args.records_only:
            build_started = time.monotonic()
            print(f"CPUS {args.jobs}", flush=True)
        try:
            records, measured, probes = _records(corpus)
            findings.extend(records)
            counts.update(measured)
            pointers, counts["lfs_pointers"] = _lfs(corpus)
            findings.extend(pointers)
        finally:
            if build_started is not None:
                _time_line("census", build_started)
        if not args.records_only and not findings:
            subset = args.subset or engine / "scripts/corpus_parity_subset.txt"
            _strategy_targets(probes)
            build_probes = probes
            if shard is not None:
                build_probes = set(shard_slice(probes, *shard))
                if not build_probes:
                    raise ValueError(f"--shard {args.shard} has no probes")
            selected_all = read_subset(subset, probes)
            selected = selected_all
            if shard is not None:
                selected = [name for name in selected_all if name in build_probes]
            for name in sorted(probes):
                for filename in ("generated.cpp", "strategy.pine"):
                    try:
                        _local_file(corpus, corpus / name / filename)
                    except ValueError as problem:
                        findings.append(f"build {corpus / name / filename}: {problem}")
            if not findings:
                if shard is not None:
                    print(
                        f"SHARD {args.shard} probes={len(build_probes)}/{len(probes)} "
                        f"selected={len(selected)}/{len(selected_all)}", flush=True)
                work.mkdir(parents=True, exist_ok=True)
                result, error = _build_and_run(
                    corpus, engine, work, build_probes, selected, args.jobs,
                    shard=shard, started=build_started)
                findings.extend(result)
    except Exception as problem:
        error = True
        message = " ".join(str(problem).splitlines())
        findings.append(f"setup {corpus if corpus is not None else 'arguments'}: "
                        f"{type(problem).__name__}: {message}")
    if status_ready:
        try:
            after = _git(corpus, "status", "--porcelain", "--untracked-files=all")
            clean = not after
            if after:
                error = True
            for line in os.fsdecode(after).splitlines():
                findings.append(f"clean {corpus}: checkout is dirty after validation: {line}")
        except Exception as problem:
            error = True
            message = " ".join(str(problem).splitlines())
            findings.append(f"clean {corpus}: could not read final status: "
                            f"{type(problem).__name__}: {message}")
    if build_started is not None:
        _time_line("total", build_started)
    print(f"ENGINE {engine_commit}")
    if joining:
        print("JOIN shards=0/- probes=- selected=-")
    else:
        print("COUNT " + " ".join(f"{name}={counts[name]}" for name in (
            "tapes", "owned_entries", "owned_hashes", "lfs_pointers", "feed", "tracked", "bound")))
    for finding in findings:
        print(f"FINDING {finding}")
    if clean:
        print("CLEAN corpus tree unchanged")
    code = 2 if error else 1 if findings else 0
    print(f"RESULT {('valid', 'invalid', 'error')[code]}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
