"""Lab-only frozen2 RED controls and byte-exact TA comparison. Never run on the Mac.

All generated files stay in this worktree's scratch-s4e. A compile failure or
a semantic/setup failure is a FAILED control, never an accepted RED. Baseline
drawings use only frozen2 headers; history also compiles frozen2 TA sources.
The native driver is linked from the named library, whose native header epoch
is unchanged by this lane. No corpus, build-system edit or mutation is run.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile

BASE = "c82fd00e8d5de30fd5d959c3e3fbcca0f0c48e9b"
TA_SOURCES = (
    "src/ta_moving_averages.cpp", "src/ta_oscillators.cpp",
    "src/ta_misc.cpp", "src/ta_volatility_trend.cpp",
    "src/ta_extremes_volume.cpp", "src/source/pine_ta_length.cpp",
    "src/run_failure.cpp",
)


def checked(command: list[str], log: Path, *, cwd: Path) -> subprocess.CompletedProcess:
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    log.write_bytes(result.stdout)
    first = next(iter(result.stdout.decode(errors="replace").splitlines()), "none")
    print(f"A5 CONTROL {log.stem} rc={result.returncode}; first diagnostic: {first}", flush=True)
    if result.returncode:
        raise RuntimeError(f"failed control: {shlex.join(command)}; log={log}\n"
                           + result.stdout.decode(errors="replace")[-12000:])
    return result


def freeze(root: Path, output: Path) -> Path:
    frozen = output / "frozen2"
    frozen.mkdir(exist_ok=True)
    result = subprocess.run(["git", "archive", BASE, "include", "src", "cmake", "VERSION"],
                            cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    (output / "archive.log").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError("cannot read frozen2 source closure: "
                           + result.stderr.decode(errors="replace"))
    # No temp directory or implicit extraction outside the worktree. Git's
    # archive is trusted source, but reject non-files and escaping names too.
    with tarfile.open(fileobj=io.BytesIO(result.stdout)) as archive:
        for member in archive.getmembers():
            destination = frozen / member.name
            destination.resolve().relative_to(frozen.resolve())
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                destination.parent.mkdir(parents=True, exist_ok=True)
                data = archive.extractfile(member)
                if data is None:
                    raise RuntimeError(f"missing frozen source: {member.name}")
                destination.write_bytes(data.read())
            else:
                raise RuntimeError(f"non-file in frozen source closure: {member.name}")
    print(f"A5 CONTROL frozen complete include/src closure base={BASE}", flush=True)
    return frozen


def flags_for(build: Path, source: Path, compiler: Path) -> list[str]:
    entries = json.loads((build / "compile_commands.json").read_text())
    matches = [entry for entry in entries if Path(entry["file"]).resolve() == source]
    if len(matches) != 1:
        raise RuntimeError(f"need one compile command for {source}, found {len(matches)}")
    entry = matches[0]
    command = entry.get("arguments") or shlex.split(entry["command"])
    start = next(index for index, word in enumerate(command)
                 if Path(word).resolve() == compiler.resolve()) + 1
    flags: list[str] = []
    index = start
    while index < len(command):
        word = command[index]
        if word in {"-o", "-MF", "-MT", "-MQ"}:
            index += 2
            continue
        if (word not in {"-c", "-MD", "-MMD", "-MP"} and
                Path(entry["directory"], word).resolve() != source):
            flags.append(word)
        index += 1
    return flags


def compile_probe(args, root: Path, output: Path, tree: Path, source: str,
                  name: str, flags: list[str], *, history: bool) -> Path:
    generated = output / (tree.name + "-generated")
    generated.mkdir(exist_ok=True)
    checked([str(args.cmake), f"-DA5_TREE={tree}", f"-DA5_GENERATED={generated}",
             "-P", str(root / "tests/a5histdraw_version.cmake")],
            output / (name + "-version.log"), cwd=root)
    executable = output / name
    discard_unused = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    command = [str(args.compiler), "-I" + str(tree / "include"), "-I" + str(generated),
               *flags, "-ffunction-sections", "-fdata-sections", discard_unused,
               str(root / "tests" / source)]
    if history:
        command.extend(str(tree / item) for item in TA_SOURCES)
    command.extend([str(args.library), "-pthread", "-o", str(executable)])
    checked(command, output / (name + "-compile.log"), cwd=args.build)
    return executable


def run_probe(executable: Path, output: Path) -> subprocess.CompletedProcess:
    result = subprocess.run([str(executable)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    (output / (executable.name + "-run.log")).write_bytes(result.stdout)
    print(result.stdout.decode(errors="replace"), end="", flush=True)
    return result


def drawings(args, root: Path, output: Path, frozen: Path) -> None:
    # Compile ALL new drawing TUs on the frozen closure before running any.
    binaries = []
    for suffix in ("100k", "1m"):
        source = f"test_drawing_growth_a5_{suffix}.cpp"
        flags = flags_for(args.build, root / "tests" / source, args.compiler)
        binary = compile_probe(args, root, output, frozen, source,
                               "base-draw-" + suffix, flags, history=False)
        binaries.append((100000 if suffix == "100k" else 1000000, binary))
    source = "test_drawing_versions_a5.cpp"
    flags = flags_for(args.build, root / "tests" / source, args.compiler)
    versions = compile_probe(args, root, output, frozen, source,
                             "base-draw-versions", flags, history=False)
    for count, executable in binaries:
        ran = run_probe(executable, output)
        text = ran.stdout.decode(errors="replace")
        pattern = (r"^A5 DRAW bars=(\d+) external_order=(\d+) retained=(\d+) "
                   r"execution_peak=(\d+) first_snapshot_peak=(\d+) restore_peak=(\d+) bound=(\d+)$")
        rows = [tuple(map(int, row)) for row in re.findall(pattern, text, re.MULTILINE)]
        if ran.returncode != 1 or [(row[0], row[1]) for row in rows] != [(count, 0), (count, 1)]:
            raise RuntimeError(f"frozen drawing control failed before its numerical RED: {executable}")
        page, pages, width, payload = 4096, 64, 32, 96
        bound = (4 * (pages + 3) * page + 5 * width * (payload + 1) +
                 4 * (17 + 11 + 13 + 7) * 4 + 2 * (payload + 1))
        failures = 0
        for _, external, retained, execution, snapshot, restore, ceiling in rows:
            expected = bound + external * 4 * (pages + 3) * page
            if ceiling != expected or not all(value > ceiling for value in
                                               (retained, execution, snapshot, restore)):
                raise RuntimeError("frozen drawings did not cross all four design-derived ceilings")
            if snapshot <= retained:
                raise RuntimeError("frozen drawing FIRST snapshot did not expose its full copy")
            failures += sum(value > ceiling for value in (retained, execution, snapshot, restore))
        roster = [tuple(map(int, row)) for row in re.findall(
            r"^A5 DRAW roster cap=(\d+) allocation_delta=(\d+)$", text, re.MULTILINE)]
        if [row[0] for row in roster] != [1, 2, 17, 500] or any(row[1] == 0 for row in roster):
            raise RuntimeError("frozen deque control did not observe runtime roster allocation")
        failures += len(roster)
        final = re.findall(r"^A5 DRAW bars=\d+ failures=(\d+)$", text, re.MULTILINE)
        if final != [str(failures)] or "semantic/setup failure" in text:
            raise RuntimeError("frozen drawing failure receipt includes an unrelated failure")
    ran = run_probe(versions, output)
    if ran.returncode:
        raise RuntimeError("frozen drawing semantic oracle failed")
    print("A5 CONTROL drawings: frozen2 resident/roster RED; semantic controls held", flush=True)


def history(args, root: Path, output: Path, frozen: Path) -> None:
    flags = flags_for(args.build, root / "tests/test_history_preallocation_a5_100k.cpp",
                      args.compiler)
    # Current producer TU + complete frozen TA/header closure: no old binary,
    # candidate TA object, or implicit PivotLevels vector conversion can hide drift.
    preparations = []
    for suffix in ("100k", "1m"):
        source = f"test_history_preallocation_a5_{suffix}.cpp"
        preparations.append(compile_probe(args, root, output, frozen, source,
                                          "base-history-" + suffix, flags, history=True))
    for executable in preparations:
        ran = run_probe(executable, output)
        text = ran.stdout.decode(errors="replace")
        rows = {name: (int(count), verdict) for name, count, verdict in re.findall(
            r"^A5 (.+) allocations=(\d+) (RED|PASS)$", text, re.MULTILINE)}
        required = ("U13 Series first push and checkpoint", "U14 source and fill checkpoint",
                    "U16 FirstCallBound late execution", "U17 HeldHistory and checkpoint")
        if ran.returncode != 1 or any(name not in rows or rows[name][0] == 0 or
                                     rows[name][1] != "RED" for name in required):
            raise RuntimeError("frozen history preparation did not reach the required allocation RED")
        for name in ("positive control EMA", "positive control RMA"):
            if rows.get(name) != (0, "PASS"):
                raise RuntimeError(f"frozen scalar positive control failed: {name}")
        count = 1000000 if executable.name.endswith("1m") else 100000
        samples = re.findall(r"^A5 bars=(\d+) sample=(\d+) allocation_delta=(\d+) "
                             r"live=(\d+) prepared_live=(\d+)$", text, re.MULTILINE)
        if not samples or tuple(map(int, samples[-1][:2])) != (count, count):
            raise RuntimeError("frozen history did not reach its claimed bar population")
        if int(samples[-1][2]) == 0 or "A5 history host:" in text:
            raise RuntimeError("frozen history RED is not from executed history allocations")
    tapes = []
    for label, tree in (("base", frozen), ("candidate", root)):
        executable = compile_probe(args, root, output, tree, "a5histdraw_numeric_probe.cpp",
                                   label + "-numeric", flags, history=True)
        tape = output / (label + "-numeric.bin")
        log = output / (label + "-numeric-run.log")
        with tape.open("wb") as data, log.open("wb") as error:
            ran = subprocess.run([str(executable)], stdout=data, stderr=error, check=False)
        if ran.returncode or not tape.stat().st_size:
            raise RuntimeError(f"numeric producer failed or empty: {executable}; {log}")
        tapes.append(tape.read_bytes())
    before, after = tapes
    if before != after:
        offset = next((i for i, pair in enumerate(zip(before, after)) if pair[0] != pair[1]),
                      min(len(before), len(after)))
        raise RuntimeError(f"TA numeric bytes differ at byte {offset} / double {offset // 8}; "
                           f"base={len(before)} candidate={len(after)}")
    print(f"A5 CONTROL numeric byte equality bytes={len(before)} "
          f"sha256={hashlib.sha256(before).hexdigest()} base={BASE}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("drawings", "history"), required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.compiler = args.compiler.resolve()
    args.cmake = args.cmake.resolve()
    args.library = args.library.resolve()
    root = Path(__file__).resolve().parent.parent
    output = root / "scratch-s4e" / ("a5histdraw-" + args.kind + "-" + args.build.name)
    output.mkdir(parents=True, exist_ok=True)
    frozen = freeze(root, output)
    if args.kind == "drawings":
        drawings(args, root, output, frozen)
    else:
        history(args, root, output, frozen)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
