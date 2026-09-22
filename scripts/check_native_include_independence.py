#!/usr/bin/env python3
"""Prove that installed native consumers do not reach source/Pine headers.

The caller supplies a disposable installation prefix. The checker installs the
configured build there, removes the source-only header trees, then compiles the
native public roots and every top-level native example (the .c one with the C
compiler) using only that installed include root. It never links or runs a
consumer binary.

With --kernel-archive the same run also reads the kernel-only static library
with nm: every defined and undefined symbol in it must be free of the source
layer, which is the link-time half of the same claim.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ROOT_HEADERS = (
    "native_host.hpp",
    "native_order.hpp",
    "native_toolkit.hpp",
    "native_run_spec.hpp",
    "native_calendar.hpp",
    "execution.hpp",
    "market_driver.hpp",
)
# Every top-level Pine-free example (PINEFORGE_BUILD_EXAMPLES), C++ and C:
# hello_kernel is the minimal host and hello_kernel_c.c its C twin (compiled
# by the C compiler below, so a C++ construct leaking into the C API header
# is caught here as well); native_market_example is exercised by
# test_native_example_batch and native_live_startup_e2e; native_selected_example
# is the R4-B second native host example. Both of the latter are also built as
# the live runner's MODULE targets. native_bracket_strategy is the L7b
# anchored-bracket host (a standalone program only). runner/examples/strategy.cpp is
# intentionally legacy/source-bound after L1. The tooling test pins this list
# against the directory, so a new example cannot be added without being
# checked here.
NATIVE_EXAMPLES = (
    ("hello-kernel", "examples/native/hello_kernel.cpp"),
    ("hello-kernel-c", "examples/native/hello_kernel_c.c"),
    ("native-market", "examples/native/native_market_strategy.cpp"),
    ("native-selected", "examples/native/native_selected_strategy.cpp"),
    ("native-bracket", "examples/native/native_bracket_strategy.cpp"),
    # One host per landed R5 lane family (audit gap N4): L2/L3 sizing + report,
    # L4/L4b margin + liquidation, L5 calculation timing, L6/L6c/L6d
    # higher-timeframe series, L7/L9 trail + risk limits.
    ("native-sized-report", "examples/native/native_sized_report_strategy.cpp"),
    ("native-margin", "examples/native/native_margin_strategy.cpp"),
    ("native-calc-on-fills", "examples/native/native_calc_on_fills_strategy.cpp"),
    ("native-htf", "examples/native/native_htf_strategy.cpp"),
    ("native-trail-risk", "examples/native/native_trail_risk_strategy.cpp"),
    # The two kernel features the Pine adapter never declares, native-only by
    # ruling (audit lane P6): the L8/L8b price grid, in C++ and from C, and the
    # L9 money limits with the kernel's own flatten.
    ("native-price-grid", "examples/native/native_price_grid_strategy.cpp"),
    ("native-price-grid-c", "examples/native/native_price_grid_c.c"),
    ("native-risk-limits", "examples/native/native_risk_limits_strategy.cpp"),
    ("native-auxiliary-feed", "examples/native/native_auxiliary_feed_strategy.cpp"),
    ("native-open-lots", "examples/native/native_open_lots_strategy.cpp"),
    ("native-fee-reserve", "examples/native/native_fee_reserve_strategy.cpp"),
    ("native-fx-roll", "examples/native/native_fx_roll_strategy.cpp"),
    ("native-broker-hash", "examples/native/native_broker_hash_strategy.cpp"),
)
NATIVE_EXAMPLES_DIRECTORY = "examples/native"
C_SOURCE_SUFFIXES = (".c",)
FORBIDDEN_DEPENDENCY_PARTS = ("/pineforge/source/", "/pineforge/compat/pine/")
FORBIDDEN_SYMBOLS = ("pineforge::source", "compat::pine")
# No exception: the rich begin bridge carries its host overrides as an opaque
# `const void*`, so no kernel signature names the source layer at all.
INCLUDE_VALUE_OPTIONS = {
    "-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros",
    "-isysroot", "-iframework", "-F",
}
OUTPUT_VALUE_OPTIONS = {"-o", "-MF", "-MT", "-MQ", "-MJ"}
DROP_STANDALONE_OPTIONS = {"-c", "-M", "-MM", "-MD", "-MMD", "-MP"}


class InfrastructureError(RuntimeError):
    """A missing build/tool or unrelated compiler failure; never expect-pass it."""


@dataclass(frozen=True)
class Finding:
    kind: str
    subject: str
    detail: str


def read_cmake_cache(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise InfrastructureError("CMakeCache.txt missing: " + str(path))
    values: dict[str, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or ":" not in line or "=" not in line:
            continue
        name_type, value = line.split("=", 1)
        values[name_type.split(":", 1)[0]] = value
    return values


def remove_forbidden_prefix_trees(prefix: Path) -> list[Path]:
    """Remove exactly the two installed source-only trees, never a broader path."""
    base = prefix / "include" / "pineforge"
    removed: list[Path] = []
    for relative in (Path("source"), Path("compat") / "pine"):
        target = base / relative
        if not target.exists():
            continue
        if target.is_symlink() or not target.is_dir():
            raise InfrastructureError("refusing non-directory forbidden prefix target: " + str(target))
        shutil.rmtree(target)
        removed.append(target)
    return removed


def _is_include_option(argument: str) -> bool:
    return argument.startswith(("-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-isysroot", "-iframework"))


def _is_output_option(argument: str) -> bool:
    return argument.startswith(("-o", "-MF", "-MT", "-MQ", "-MJ"))


def _looks_like_source_operand(argument: str, source_file: str | None) -> bool:
    if source_file and Path(argument).name == Path(source_file).name:
        return True
    return argument.endswith((".cc", ".cp", ".cxx", ".cpp", ".C", ".c"))


def sanitize_compile_flags(arguments: list[str], *, source_file: str | None = None,
                           compiler: str | None = None) -> list[str]:
    """Keep compiler semantics but remove source/build include and output paths."""
    retained: list[str] = []
    index = 0
    compiler_name = Path(compiler).name if compiler else ""
    while index < len(arguments):
        argument = arguments[index]
        if index == 0 and (not compiler_name or Path(argument).name == compiler_name):
            index += 1
            continue
        if argument in INCLUDE_VALUE_OPTIONS or argument in OUTPUT_VALUE_OPTIONS:
            index += 2
            continue
        if _is_include_option(argument) or _is_output_option(argument):
            index += 1
            continue
        if argument in DROP_STANDALONE_OPTIONS or _looks_like_source_operand(argument, source_file):
            index += 1
            continue
        retained.append(argument)
        index += 1
    return retained


def _toolchain_flags(build_dir: Path, cache: dict[str, str], *, language: str,
                     preferred_files: tuple[str, ...], suffixes: tuple[str, ...],
                     default_standard: str) -> tuple[str, list[str], str]:
    """The configured compiler and sanitized flags of one language (CXX or C).

    The flags come from a compile command of that language when the build
    exported one, else from the cache's per-language flag variables."""
    compiler_key = "CMAKE_" + language + "_COMPILER"
    compiler = cache.get(compiler_key)
    if not compiler:
        raise InfrastructureError(compiler_key + " is missing from CMakeCache.txt")
    database = build_dir / "compile_commands.json"
    if database.is_file():
        try:
            entries = json.loads(database.read_text())
        except (OSError, ValueError) as error:
            raise InfrastructureError("cannot read compile_commands.json: " + str(error)) from error
        if isinstance(entries, list):
            entry = None
            for name in preferred_files:
                entry = next((candidate for candidate in entries
                              if Path(str(candidate.get("file", ""))).name == name), None)
                if entry is not None:
                    break
            entry = entry or next((candidate for candidate in entries
                                   if str(candidate.get("file", "")).endswith(suffixes)), None)
            if isinstance(entry, dict):
                raw = entry.get("arguments")
                if not isinstance(raw, list):
                    command = entry.get("command")
                    raw = shlex.split(command) if isinstance(command, str) else None
                if isinstance(raw, list) and raw:
                    return (compiler,
                            sanitize_compile_flags([str(part) for part in raw],
                                                   source_file=str(entry.get("file", "")),
                                                   compiler=compiler),
                            "compile_commands.json")
    build_type = cache.get("CMAKE_BUILD_TYPE", "").upper()
    flags = shlex.split(cache.get("CMAKE_" + language + "_FLAGS", ""))
    if build_type:
        flags += shlex.split(cache.get("CMAKE_" + language + "_FLAGS_" + build_type, ""))
    flags = sanitize_compile_flags(flags, compiler=compiler)
    if not any(flag.startswith("-std=") for flag in flags):
        flags.append(default_standard)
    return compiler, flags, "CMakeCache.txt"


def compile_command_flags(build_dir: Path, cache: dict[str, str]) -> tuple[str, list[str], str]:
    """The C++ toolchain: the root headers and every .cpp example compile with it."""
    return _toolchain_flags(
        build_dir, cache, language="CXX",
        preferred_files=("native_market_strategy.cpp", "c_abi.cpp"),
        suffixes=(".cc", ".cp", ".cxx", ".cpp", ".C"),
        default_standard="-std=c++17")


def c_compile_command_flags(build_dir: Path, cache: dict[str, str]) -> tuple[str, list[str], str]:
    """The C toolchain: a .c example must compile as C, never as C++ with a
    C++ standard flag, or a C++ construct in the C API header would pass."""
    return _toolchain_flags(
        build_dir, cache, language="C",
        preferred_files=("hello_kernel_c.c", "test_native_c_api.c", "test_c_abi.c"),
        suffixes=C_SOURCE_SUFFIXES,
        default_standard="-std=c11")


def is_c_source(relative: str) -> bool:
    return relative.endswith(C_SOURCE_SUFFIXES)


def example_sources_on_disk(root: Path = ROOT) -> tuple[str, ...]:
    """Every C/C++ source under examples/native/, as the manifest spells them."""
    directory = root / NATIVE_EXAMPLES_DIRECTORY
    return tuple(sorted(
        str(path.relative_to(root)) for path in directory.iterdir()
        if path.is_file() and path.suffix in {".c", ".cc", ".cp", ".cxx", ".cpp", ".C"}))


def parse_depfile(path: Path) -> list[str]:
    if not path.is_file():
        return []
    text = re.sub(r"\\\r?\n", " ", path.read_text(errors="replace"))
    if ":" not in text:
        return []
    dependencies = text.split(":", 1)[1]
    tokens = re.findall(r"(?:\\.|[^\s])+", dependencies)
    return [re.sub(r"\\([ \\])", r"\1", token) for token in tokens]


def forbidden_dependency_entries(entries: list[str]) -> list[str]:
    found = []
    for entry in entries:
        normalized = entry.replace("\\", "/")
        if any(part in normalized for part in FORBIDDEN_DEPENDENCY_PARTS):
            found.append(entry)
    return found


def forbidden_symbol_lines(symbols: str) -> list[str]:
    return [line for line in symbols.splitlines()
            if any(token in line for token in FORBIDDEN_SYMBOLS)]


def kernel_archive_findings(archive: Path, *, evidence_dir: Path | None = None) -> list[Finding]:
    """nm the kernel archive: neither its definitions nor its undefined
    references may name the source layer. An undefined source symbol would
    make the archive unlinkable on its own, which is exactly what the
    kernel-only build promises it is not."""
    if not archive.is_file():
        raise InfrastructureError("kernel archive is missing: " + str(archive))
    listed = run_command(["nm", "-C", str(archive)], label="nm " + str(archive), timeout=120)
    if listed.returncode:
        raise InfrastructureError("nm " + str(archive) + " failed:\n"
                                  + listed.stdout + listed.stderr)
    archive_text(evidence_dir, Path("nm") / "kernel-archive.txt", listed.stdout)
    return [Finding("symbol", archive.name, line)
            for line in forbidden_symbol_lines(listed.stdout)]


def independence_exit_code(findings: list[Finding], *, expect_fail: bool) -> int:
    if expect_fail:
        return 0 if findings else 1
    return 1 if findings else 0


def run_command(argv: list[str], *, label: str, timeout: int = 180) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(argv, text=True, capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise InfrastructureError(label + " could not run: " + str(error)) from error


def compile_object(compiler: str, flags: list[str], *, source: Path, output: Path,
                   depfile: Path, include_root: Path, label: str) -> subprocess.CompletedProcess[str]:
    output.parent.mkdir(parents=True, exist_ok=True)
    depfile.parent.mkdir(parents=True, exist_ok=True)
    argv = [compiler, *flags, "-I", str(include_root), "-MMD", "-MF", str(depfile),
            "-c", str(source), "-o", str(output)]
    result = run_command(argv, label=label, timeout=120)
    if result.returncode == 0 and not output.is_file():
        raise InfrastructureError(label + " reported success without producing " + str(output))
    return result


def compiler_failure_finding(label: str, result: subprocess.CompletedProcess[str]) -> Finding | None:
    diagnostic = (result.stdout + result.stderr).strip()
    if any(token in diagnostic.replace("\\", "/") for token in ("compat/pine/", "pineforge/source/")):
        return Finding("compile", label, diagnostic or "forbidden include prevented compilation")
    return None


def generic_consumer_source() -> str:
    return """#include <pineforge/series.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/native_calendar.hpp>
int main() {
    pineforge::Series<double> series(2);
    series.push(1.0);
    pineforge::ta::SMA sma(2);
    (void)sma.compute(series.current());
    auto timeframe = pineforge::native_calendar::parse_timeframe("1");
    auto calendar = pineforge::native_calendar::parse_session("24x7", "UTC");
    return (!timeframe || !calendar) ? 1 : 0;
}
"""


def format_findings(findings: list[Finding]) -> str:
    lines = ["native include independence: FAILED"]
    for finding in findings:
        lines.append(f"[{finding.kind}] {finding.subject}")
        lines.extend("  " + line for line in finding.detail.splitlines() if line)
    return "\n".join(lines)


def archive_text(evidence_dir: Path | None, relative: Path, text: str) -> None:
    if evidence_dir is None:
        return
    destination = evidence_dir / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text)


def check(build_dir: Path, prefix: Path, *, expect_fail: bool = False,
          evidence_dir: Path | None = None, kernel_archive: Path | None = None) -> int:
    build_dir = build_dir.resolve()
    prefix = prefix.resolve()
    evidence_dir = evidence_dir.resolve() if evidence_dir is not None else None
    if not (build_dir / "CMakeCache.txt").is_file():
        raise InfrastructureError("build directory is not configured: " + str(build_dir))
    if prefix in {ROOT, build_dir} or ROOT in prefix.parents or build_dir in prefix.parents:
        raise InfrastructureError("--prefix must be a disposable path outside source and build directories")
    installed = run_command(["cmake", "--install", str(build_dir), "--prefix", str(prefix)],
                            label="cmake install", timeout=180)
    if installed.returncode:
        raise InfrastructureError("cmake install failed:\n" + installed.stdout + installed.stderr)
    include_root = prefix / "include"
    if not (include_root / "pineforge").is_dir():
        raise InfrastructureError("install did not produce " + str(include_root / "pineforge"))
    remove_forbidden_prefix_trees(prefix)
    cache = read_cmake_cache(build_dir / "CMakeCache.txt")
    compiler, flags, origin = compile_command_flags(build_dir, cache)
    c_compiler, c_flags, c_origin = c_compile_command_flags(build_dir, cache)
    if evidence_dir is not None:
        evidence_dir.mkdir(parents=True, exist_ok=True)
        archive_text(evidence_dir, Path("manifest.json"), json.dumps({
            "buildDir": str(build_dir),
            "compiler": compiler,
            "flags": flags,
            "flagsOrigin": origin,
            "cCompiler": c_compiler,
            "cFlags": c_flags,
            "cFlagsOrigin": c_origin,
            "rootHeaders": list(ROOT_HEADERS),
            "nativeExamples": [relative for _, relative in NATIVE_EXAMPLES],
        }, indent=2, sort_keys=True) + "\n")
    if not shutil.which(compiler) and not Path(compiler).is_file():
        raise InfrastructureError("C++ compiler not found: " + compiler)
    if not shutil.which(c_compiler) and not Path(c_compiler).is_file():
        raise InfrastructureError("C compiler not found: " + c_compiler)
    unlisted = sorted(set(example_sources_on_disk()) - {relative for _, relative in NATIVE_EXAMPLES})
    if unlisted:
        raise InfrastructureError("examples not covered by NATIVE_EXAMPLES: " + ", ".join(unlisted))
    findings: list[Finding] = []
    with tempfile.TemporaryDirectory(prefix="pineforge-native-include-") as temporary:
        work = Path(temporary)
        for header in ROOT_HEADERS:
            source = work / (header + ".cpp")
            source.write_text("#include <pineforge/" + header + ">\nint main() { return 0; }\n")
            result = compile_object(compiler, flags, source=source,
                                    output=work / "objects" / (header + ".o"),
                                    depfile=work / "deps" / (header + ".d"),
                                    include_root=include_root, label="compile " + header)
            depfile = work / "deps" / (header + ".d")
            if depfile.is_file():
                archive_text(evidence_dir, Path("dependencies") / (header + ".d"),
                             depfile.read_text(errors="replace"))
            if result.returncode:
                finding = compiler_failure_finding("compile " + header, result)
                if finding is None:
                    raise InfrastructureError("compile " + header + " failed:\n" + result.stdout + result.stderr)
                findings.append(finding)
                continue
            for dependency in forbidden_dependency_entries(parse_depfile(depfile)):
                findings.append(Finding("dependency", header, dependency))
        for name, relative in NATIVE_EXAMPLES:
            source = ROOT / relative
            if not source.is_file():
                raise InfrastructureError("native example is missing: " + str(source))
            output = work / "objects" / (name + ".o")
            example_compiler, example_flags = (
                (c_compiler, c_flags) if is_c_source(relative) else (compiler, flags))
            result = compile_object(example_compiler, example_flags, source=source, output=output,
                                    depfile=work / "deps" / (name + ".d"),
                                    include_root=include_root, label="compile " + relative)
            depfile = work / "deps" / (name + ".d")
            if depfile.is_file():
                archive_text(evidence_dir, Path("dependencies") / (name + ".d"),
                             depfile.read_text(errors="replace"))
            if result.returncode:
                finding = compiler_failure_finding("compile " + relative, result)
                if finding is None:
                    raise InfrastructureError("compile " + relative + " failed:\n" + result.stdout + result.stderr)
                findings.append(finding)
                continue
            for dependency in forbidden_dependency_entries(parse_depfile(depfile)):
                findings.append(Finding("dependency", relative, dependency))
            nm = run_command(["nm", "-C", str(output)], label="nm " + relative, timeout=60)
            if nm.returncode:
                raise InfrastructureError("nm " + relative + " failed:\n" + nm.stdout + nm.stderr)
            archive_text(evidence_dir, Path("nm") / (name + ".txt"), nm.stdout)
            for line in forbidden_symbol_lines(nm.stdout):
                findings.append(Finding("symbol", relative, line))
        generic = work / "generic-consumer.cpp"
        generic.write_text(generic_consumer_source())
        result = compile_object(compiler, flags, source=generic,
                                output=work / "objects" / "generic-consumer.o",
                                depfile=work / "deps" / "generic-consumer.d",
                                include_root=include_root, label="compile standalone Series/TA/calendar consumer")
        generic_depfile = work / "deps" / "generic-consumer.d"
        if generic_depfile.is_file():
            archive_text(evidence_dir, Path("dependencies") / "generic-consumer.d",
                         generic_depfile.read_text(errors="replace"))
        if result.returncode:
            raise InfrastructureError("standalone Series/TA/calendar consumer failed:\n"
                                      + result.stdout + result.stderr)
        for dependency in forbidden_dependency_entries(parse_depfile(generic_depfile)):
            findings.append(Finding("dependency", "standalone Series/TA/calendar consumer", dependency))
    if kernel_archive is not None:
        findings.extend(kernel_archive_findings(kernel_archive.resolve(),
                                                evidence_dir=evidence_dir))
    if findings:
        print(format_findings(findings))
    else:
        print("native include independence: passed (flags from " + origin + ")")
    if expect_fail and not findings:
        print("native include independence: --expect-fail expected a forbidden dependency or symbol")
    return independence_exit_code(findings, expect_fail=expect_fail)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path,
                        help="optional directory for dependency files and native-example nm output")
    parser.add_argument("--kernel-archive", type=Path, default=None,
                        help="optional libpineforge_kernel.a to assert free of source-layer symbols")
    parser.add_argument("--expect-fail", action="store_true",
                        help="succeed only when a genuine forbidden dependency/symbol is found")
    args = parser.parse_args(argv)
    try:
        return check(args.build_dir, args.prefix, expect_fail=args.expect_fail,
                     evidence_dir=args.evidence_dir, kernel_archive=args.kernel_archive)
    except InfrastructureError as error:
        print("native include independence: infrastructure failure: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
