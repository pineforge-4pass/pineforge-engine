#!/usr/bin/env python3
"""Build PineForge's ordinary native x86-64 GNU 13.3.0 Release tree.

This helper deliberately owns no fallback toolchain and no test invocation.
It verifies the host and compiler first, configures the repository's ordinary
all-target build, builds through Unix Makefiles, and retains the primary
configure/build evidence for the workflow artifact.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import TextIO


EXPECTED_MACHINE = "x86_64"
EXPECTED_VERSION = "13.3.0"
COMPILER_PAIRS = (
    ("gcc-13.3.0", "g++-13.3.0"),
    ("gcc-13", "g++-13"),
    ("gcc", "g++"),
)


class ToolchainError(RuntimeError):
    """The runner does not expose the required native compiler pair."""


@dataclass(frozen=True)
class Compiler:
    language: str
    path: Path
    target: str
    version: str
    banner: str


@dataclass(frozen=True)
class Toolchain:
    c: Compiler
    cxx: Compiler


def _run_text(argv: list[str]) -> str:
    try:
        result = subprocess.run(argv, check=False, capture_output=True, text=True)
    except OSError as error:
        raise ToolchainError(f"cannot execute {' '.join(argv)}: {error}") from error
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise ToolchainError(
            f"{' '.join(argv)} exited {result.returncode}: {detail or 'no diagnostic'}"
        )
    return result.stdout.strip()


def _compiler(path: Path, language: str) -> Compiler:
    target = _run_text([str(path), "-dumpmachine"])
    version = _run_text([str(path), "-dumpfullversion"])
    banner = _run_text([str(path), "--version"])
    if not target.startswith(f"{EXPECTED_MACHINE}-") and target != EXPECTED_MACHINE:
        raise ToolchainError(f"{path} targets {target!r}, not native {EXPECTED_MACHINE}")
    if version != EXPECTED_VERSION:
        raise ToolchainError(f"{path} reports GCC {version!r}, not {EXPECTED_VERSION}")
    return Compiler(language, path.resolve(), target, version, banner)


def select_toolchain() -> Toolchain:
    """Select the first available pair that independently passes every check."""
    machine = _run_text(["uname", "-m"])
    if machine != EXPECTED_MACHINE:
        raise ToolchainError(f"uname reports {machine!r}, not native {EXPECTED_MACHINE}")

    failures: list[str] = []
    for c_name, cxx_name in COMPILER_PAIRS:
        c_path = shutil.which(c_name)
        cxx_path = shutil.which(cxx_name)
        if c_path is None or cxx_path is None:
            missing = [name for name, path in ((c_name, c_path), (cxx_name, cxx_path))
                       if path is None]
            failures.append(f"{c_name}/{cxx_name}: missing {', '.join(missing)}")
            continue
        try:
            c = _compiler(Path(c_path), "C")
            cxx = _compiler(Path(cxx_path), "CXX")
        except ToolchainError as error:
            failures.append(f"{c_name}/{cxx_name}: {error}")
            continue
        if c.target != cxx.target:
            failures.append(
                f"{c_name}/{cxx_name}: target mismatch {c.target!r} and {cxx.target!r}"
            )
            continue
        return Toolchain(c, cxx)
    detail = "; ".join(failures) if failures else "no compiler candidates were available"
    raise ToolchainError(
        f"native GCC/G++ {EXPECTED_VERSION} toolchain is unavailable: {detail}"
    )


def _write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")


def _identity(toolchain: Toolchain | None, machine: str | None, error: str | None) -> str:
    lines = [
        "required_machine=x86_64",
        f"uname_machine={machine or 'unavailable'}",
        f"required_gcc_version={EXPECTED_VERSION}",
    ]
    if toolchain is not None:
        for compiler in (toolchain.c, toolchain.cxx):
            lines.extend((
                f"{compiler.language}_compiler={compiler.path}",
                f"{compiler.language}_compiler_target={compiler.target}",
                f"{compiler.language}_compiler_version={compiler.version}",
                f"{compiler.language}_compiler_banner:",
                compiler.banner,
            ))
    if error:
        lines.extend(("selection_error:", error))
    return "\n".join(lines) + "\n"


def _source_identity(source: Path) -> str:
    rows = [f"source={source}"]
    for argv, label in (
        (["git", "rev-parse", "HEAD"], "git_head"),
        (["git", "status", "--short"], "git_status"),
    ):
        try:
            result = subprocess.run(
                argv, cwd=source, check=False, capture_output=True, text=True
            )
            rows.append(f"{label}_exit={result.returncode}")
            rows.append(f"{label}:")
            rows.append(result.stdout.rstrip())
            if result.stderr:
                rows.append(f"{label}_stderr:")
                rows.append(result.stderr.rstrip())
        except OSError as error:
            rows.extend((f"{label}_exit=unavailable", f"{label}_error={error}"))
    return "\n".join(rows) + "\n"


def _cache_values(cache: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not cache.is_file():
        return values
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#") or ":" not in line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.split(":", 1)[0]] = value
    return values


_COMPILER_SETTING = re.compile(
    r'^set\((CMAKE_[A-Z]+_COMPILER(?:_ID|_VERSION)?)\s+"([^"]*)"\)\s*$'
)


def _compiler_records(build_dir: Path, language: str) -> list[dict[str, str]]:
    """Read every compiler description CMake generated for one language.

    CMake keeps the detected compiler identity and version in
    CMakeFiles/<cmake version>/CMake<language>Compiler.cmake, never in
    CMakeCache.txt, so the cache alone cannot show which compiler it found.
    """
    records: list[dict[str, str]] = []
    pattern = f"*/CMake{language}Compiler.cmake"
    for path in sorted((build_dir / "CMakeFiles").glob(pattern)):
        settings: dict[str, str] = {}
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = _COMPILER_SETTING.match(line.strip())
            if match:
                settings[match.group(1)] = match.group(2)
        records.append(settings)
    return records


def _verify_configure(build_dir: Path, toolchain: Toolchain) -> list[str]:
    values = _cache_values(build_dir / "CMakeCache.txt")
    expected = {
        "CMAKE_C_COMPILER": str(toolchain.c.path),
        "CMAKE_CXX_COMPILER": str(toolchain.cxx.path),
        "CMAKE_GENERATOR": "Unix Makefiles",
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_CXX_STANDARD": "17",
    }
    findings = [
        f"{key} expected {wanted!r}, got {values.get(key)!r}"
        for key, wanted in expected.items()
        if values.get(key) != wanted
    ]
    for compiler in (toolchain.c, toolchain.cxx):
        key = f"CMAKE_{compiler.language}_COMPILER"
        records = _compiler_records(build_dir, compiler.language)
        if not records:
            findings.append(
                f"no CMake {compiler.language} compiler description under "
                f"{build_dir / 'CMakeFiles'}"
            )
        for record in records:
            for name, wanted in (
                (key, str(compiler.path)),
                (f"{key}_ID", "GNU"),
                (f"{key}_VERSION", EXPECTED_VERSION),
            ):
                if record.get(name) != wanted:
                    findings.append(
                        f"detected {name} expected {wanted!r}, got {record.get(name)!r}"
                    )
    return findings


def _cmake_definitions(toolchain: Toolchain) -> list[str]:
    return [
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_STANDARD=17",
        "-DCMAKE_CXX_STANDARD_REQUIRED=ON",
        "-DCMAKE_CXX_EXTENSIONS=OFF",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DCMAKE_C_COMPILER={toolchain.c.path}",
        f"-DCMAKE_CXX_COMPILER={toolchain.cxx.path}",
        "-DPINEFORGE_VERSION_SOURCE=FILE",
        "-DPINEFORGE_BUILD_TESTS=OFF",
        "-DPINEFORGE_BUILD_SOURCE_LAYER=ON",
        "-DPINEFORGE_BUILD_EXAMPLES=OFF",
        "-DPINEFORGE_BUILD_CORPUS_STRATEGIES=OFF",
        "-DPINEFORGE_BUILD_TUTORIAL=OFF",
        "-DPINEFORGE_BUILD_LIVE_RUNNER=OFF",
        "-DPINEFORGE_BUILD_BENCH_STRATEGIES=OFF",
        "-DPINEFORGE_BUILD_SPEED_BENCH=OFF",
        "-DPINEFORGE_ENABLE_COVERAGE=OFF",
        "-DPINEFORGE_ENABLE_SANITIZERS=OFF",
        "-DPINEFORGE_STRICT_WARNINGS=OFF",
        "-DPINEFORGE_REQUIRE_ABI_RECEIPTS=OFF",
    ]


def _run_logged(
    argv: list[str],
    *,
    cwd: Path,
    env: dict[str, str],
    stdout_path: Path,
    stderr_path: Path,
    stdout: TextIO,
    stderr: TextIO,
) -> int:
    with stdout_path.open("w", encoding="utf-8") as out, \
            stderr_path.open("w", encoding="utf-8") as err:
        result = subprocess.run(
            argv, cwd=cwd, env=env, check=False, stdout=out, stderr=err
        )
    for path, stream in ((stdout_path, stdout), (stderr_path, stderr)):
        try:
            with path.open("r", encoding="utf-8", errors="replace") as saved:
                shutil.copyfileobj(saved, stream)
            stream.flush()
        except OSError:
            # The primary process result remains authoritative; collection is
            # reported separately and cannot turn a failed build into success.
            pass
    return result.returncode


def _copy_evidence(build_dir: Path, artifact_dir: Path) -> list[str]:
    errors: list[str] = []
    for name in ("CMakeCache.txt", "compile_commands.json"):
        source = build_dir / name
        destination = artifact_dir / name
        if not source.is_file():
            errors.append(f"missing {source}")
            continue
        try:
            shutil.copy2(source, destination)
        except OSError as error:
            errors.append(f"copy {source}: {error}")
    return errors


def _positive_jobs(value: str) -> int:
    try:
        jobs = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("jobs must be a positive integer") from error
    if jobs <= 0:
        raise argparse.ArgumentTypeError("jobs must be a positive integer")
    return jobs


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=_positive_jobs, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    source = args.source.resolve()
    build_dir = args.build_dir.resolve()
    artifact_dir = args.artifact_dir.resolve()
    artifact_errors: list[str] = []
    primary_exit_code: int | None = None
    toolchain: Toolchain | None = None
    machine: str | None = None
    try:
        artifact_dir.mkdir(parents=True, exist_ok=True)
        _write_text(artifact_dir / "source-identity.txt", _source_identity(source))
        try:
            machine = _run_text(["uname", "-m"])
            toolchain = select_toolchain()
            _write_text(
                artifact_dir / "compiler-identity.txt",
                _identity(toolchain, machine, None),
            )
        except ToolchainError as error:
            message = str(error)
            _write_text(
                artifact_dir / "compiler-identity.txt",
                _identity(None, machine, message),
            )
            _write_text(artifact_dir / "configure.exitcode", "not-run\n")
            _write_text(artifact_dir / "build.exitcode", "not-run\n")
            print(f"native x86 GNU 13.3.0 refusal: {message}", file=sys.stderr)
            return 1

        environment = os.environ.copy()
        environment["CC"] = str(toolchain.c.path)
        environment["CXX"] = str(toolchain.cxx.path)
        configure_argv = [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build_dir),
            "-G",
            "Unix Makefiles",
            *_cmake_definitions(toolchain),
        ]
        _write_text(
            artifact_dir / "configure.argv",
            "\n".join(configure_argv) + "\n",
        )
        build_argv = [
            "cmake",
            "--build",
            str(build_dir),
            "--parallel",
            str(args.jobs),
            "--",
            "-k",
            "VERBOSE=1",
        ]
        _write_text(artifact_dir / "build.argv", "\n".join(build_argv) + "\n")

        configure_exit = _run_logged(
            configure_argv,
            cwd=source,
            env=environment,
            stdout_path=artifact_dir / "configure.stdout",
            stderr_path=artifact_dir / "configure.stderr",
            stdout=sys.stdout,
            stderr=sys.stderr,
        )
        _write_text(artifact_dir / "configure.exitcode", f"{configure_exit}\n")
        if configure_exit:
            primary_exit_code = configure_exit
            _write_text(artifact_dir / "build.exitcode", "not-run\n")
        else:
            configure_findings = _verify_configure(build_dir, toolchain)
            if configure_findings:
                message = "\n".join(configure_findings)
                _write_text(artifact_dir / "configure.verification", message + "\n")
                print(f"configure verification failed:\n{message}", file=sys.stderr)
                primary_exit_code = 1
                _write_text(artifact_dir / "build.exitcode", "not-run\n")
            else:
                build_exit = _run_logged(
                    build_argv,
                    cwd=source,
                    env=environment,
                    stdout_path=artifact_dir / "build.stdout",
                    stderr_path=artifact_dir / "build.stderr",
                    stdout=sys.stdout,
                    stderr=sys.stderr,
                )
                _write_text(artifact_dir / "build.exitcode", f"{build_exit}\n")
                primary_exit_code = build_exit
    except (OSError, subprocess.SubprocessError) as error:
        message = f"native x86 GNU 13.3.0 helper failure: {error}"
        print(message, file=sys.stderr)
        if primary_exit_code is None:
            primary_exit_code = 1
        try:
            _write_text(artifact_dir / "helper.error", message + "\n")
        except OSError as artifact_error:
            artifact_errors.append(f"write helper.error: {artifact_error}")
    finally:
        try:
            artifact_dir.mkdir(parents=True, exist_ok=True)
            artifact_errors.extend(_copy_evidence(build_dir, artifact_dir))
        except OSError as error:
            artifact_errors.append(f"retain build evidence: {error}")
        if artifact_errors:
            reported_errors = list(artifact_errors)
            try:
                _write_text(
                    artifact_dir / "artifact.error",
                    "\n".join(reported_errors) + "\n",
                )
            except OSError as error:
                reported_errors.append(f"write artifact.error: {error}")
            for error in reported_errors:
                print(f"artifact retention failure: {error}", file=sys.stderr)

    if primary_exit_code is None:
        primary_exit_code = 1
    if primary_exit_code:
        return primary_exit_code
    return 1 if artifact_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
