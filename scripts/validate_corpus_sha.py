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


sys.dont_write_bytecode = True
from corpus_trades_identity import ALLOWED_EXTRA_COLUMNS  # noqa: E402


ENGINE = Path(__file__).resolve().parents[1]
FEED = "data/ohlcv_ETH-USDT-USDT_1m.csv"
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


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _local_file(root: Path, path: Path) -> None:
    if not path.resolve().is_relative_to(root.resolve()) or path.is_symlink():
        raise ValueError("file must stay inside the corpus and must not be a symlink")
    if not path.is_file():
        raise ValueError("file is missing")


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


def _digest_problem(root: Path, path: Path, expected: object) -> str | None:
    try:
        _local_file(root, path)
        if not isinstance(expected, str) or not DIGEST.fullmatch(expected):
            raise ValueError("recorded sha256 is missing or malformed")
        actual = _sha256(path)
        if actual != expected:
            return f"records {path}: sha256 {actual} differs from recorded {expected}"
    except (OSError, ValueError) as error:
        return f"records {path}: {error}"
    return None


def _records(root: Path) -> tuple[list[str], dict[str, int], set[str]]:
    findings: list[str] = []
    counts = dict(tapes=0, owned_entries=0, owned_hashes=0, feed=0)
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
    problem = _digest_problem(root, root / FEED, manifest.get("feedSha256"))
    if problem:
        findings.append(problem)
    else:
        counts["feed"] = 1

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
    return findings, counts, probes


def check_records(root: Path) -> list[str]:
    """Bind each manifest record to its named bytes, without requiring Git."""
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
        with path.open("rb") as stream:
            if stream.read(len(LFS_VERSION)) == LFS_VERSION:
                raise ValueError("Git-LFS content is not materialized")
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


def _build_and_run(corpus: Path, engine: Path, work: Path, probes: set[str],
                   selected: list[str], jobs: int = 8) -> tuple[list[str], bool]:
    run = Path(tempfile.mkdtemp(prefix="corpus-validation-", dir=work))
    source, build = run / "engine", run / "build"
    print(f"WORK {run}")
    _copy_engine(engine, source)
    staged = source / "corpus"
    shutil.copytree(corpus, staged, ignore=_copy_ignore)
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
    if _command(configure, source, run / "configure.log", environment):
        return [f"build {engine / 'CMakeLists.txt'}: configure failed; see {run / 'configure.log'}"], True
    if _command(["cmake", "--build", str(build), "--target", "corpus_strategies",
                 "--parallel", str(jobs)], source, run / "build.log", environment):
        return [f"build {corpus / 'validation'}: corpus_strategies failed; see {run / 'build.log'}"], False
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
        return findings, False
    derived = staged / "data" / "derived"
    if _command([sys.executable, "-B", str(source / "scripts/derive_corpus_feeds.py"),
                 "--corpus-root", str(staged), "--derived-workspace", str(derived)],
                source, run / "derive.log", environment):
        return [f"feed {corpus / FEED}: derivation failed; see {run / 'derive.log'}"], True

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

    with ThreadPoolExecutor(max_workers=jobs) as workers:
        for problem in workers.map(run_probe, selected):
            if problem:
                findings.append(problem)
    return findings, False


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
    counts = dict(tapes=0, owned_entries=0, owned_hashes=0, lfs_pointers=0, feed=0)
    findings: list[str] = []
    corpus = None
    status_ready = False
    clean = False
    error = False
    try:
        args = parser.parse_args(argv)
        corpus, engine = args.corpus.resolve(), args.engine.resolve()
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
            for line in os.fsdecode(before).splitlines():
                findings.append(f"clean {corpus}: checkout is dirty before validation: {line}")
        print(f"CORPUS {os.fsdecode(_git(corpus, 'rev-parse', 'HEAD')).strip()}")
        records, measured, probes = _records(corpus)
        findings.extend(records)
        counts.update(measured)
        pointers, counts["lfs_pointers"] = _lfs(corpus)
        findings.extend(pointers)
        if not args.records_only and not findings:
            subset = args.subset or engine / "scripts/corpus_parity_subset.txt"
            selected = read_subset(subset, probes)
            for name in sorted(probes):
                for filename in ("generated.cpp", "strategy.pine"):
                    try:
                        _local_file(corpus, corpus / name / filename)
                    except ValueError as problem:
                        findings.append(f"build {corpus / name / filename}: {problem}")
            if not findings:
                work.mkdir(parents=True, exist_ok=True)
                result, error = _build_and_run(corpus, engine, work, probes, selected, args.jobs)
                findings.extend(result)
    except (OSError, ValueError, subprocess.CalledProcessError) as problem:
        error = True
        findings.append(f"setup {corpus if corpus is not None else 'arguments'}: {problem}")
    if status_ready:
        try:
            after = _git(corpus, "status", "--porcelain", "--untracked-files=all")
            clean = not after
            for line in os.fsdecode(after).splitlines():
                findings.append(f"clean {corpus}: checkout is dirty after validation: {line}")
        except (OSError, subprocess.CalledProcessError) as problem:
            error = True
            findings.append(f"clean {corpus}: could not read final status: {problem}")
    print("COUNT " + " ".join(f"{name}={counts[name]}" for name in (
        "tapes", "owned_entries", "owned_hashes", "lfs_pointers", "feed")))
    for finding in findings:
        print(f"FINDING {finding}")
    if clean:
        print("CLEAN corpus tree unchanged")
    code = 2 if error else 1 if findings else 0
    print(f"RESULT {('valid', 'invalid', 'error')[code]}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
