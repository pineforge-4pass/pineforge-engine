#!/usr/bin/env python3
"""Codegen + compile a bench strategy.pine into a runnable .dylib/.so.

Bench source-of-truth is `assets/strategies/<NN-slug>/strategy.pine`.
This script:
    1. Reads the .pine file.
    2. Calls pineforge_codegen.transpile(pine_source) -> C++ source.
    3. Writes generated.cpp into _workdir/build_strategies/<NN-slug>/.
    4. Compiles with clang++ against libpineforge.{a,dylib} from
       <repo>/build/, mirroring corpus/CMakeLists.txt link flags.
    5. Returns absolute path of the resulting strategy.dylib.

The bench builds are isolated under _workdir so they never collide with
the corpus's pinned strategy.dylib files.
"""
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH = REPO_ROOT / "benchmarks"
BUILD_DIR = REPO_ROOT / "build"
WORKDIR = BENCH / "_workdir" / "build_strategies"
CODEGEN_REPO = REPO_ROOT.parent / "pineforge-codegen"
INCLUDE_DIR = REPO_ROOT / "include"
sys.path.insert(0, str(BENCH))
from paths import STRATEGIES  # noqa: E402

# Locate libpineforge static lib produced by the engine build.
def _find_libpineforge() -> Path:
    for cand in (
        BUILD_DIR / "libpineforge.a",
        BUILD_DIR / "lib" / "libpineforge.a",
        BUILD_DIR / "src" / "libpineforge.a",
    ):
        if cand.exists():
            return cand
    raise FileNotFoundError(
        "libpineforge.a not found under build/. Run: "
        "cmake -B build -DPINEFORGE_BUILD_TESTS=ON && cmake --build build --target pineforge -j"
    )


def _ensure_codegen_importable() -> None:
    cand = Path(os.environ.get("PINEFORGE_CODEGEN_PATH", str(CODEGEN_REPO)))
    if not cand.exists():
        raise FileNotFoundError(f"pineforge-codegen not found at {cand} (set PINEFORGE_CODEGEN_PATH)")
    if str(cand) not in sys.path:
        sys.path.insert(0, str(cand))


def transpile_pine(pine_path: Path, dest_cpp: Path) -> None:
    _ensure_codegen_importable()
    from pineforge_codegen import transpile  # type: ignore
    cpp = transpile(pine_path.read_text())
    dest_cpp.parent.mkdir(parents=True, exist_ok=True)
    dest_cpp.write_text(cpp)


def compile_dylib(cpp_path: Path, out_dylib: Path) -> None:
    libpf = _find_libpineforge()
    is_macos = platform.system() == "Darwin"
    suffix = ".dylib" if is_macos else ".so"
    if out_dylib.suffix != suffix:
        out_dylib = out_dylib.with_suffix(suffix)

    cxx = os.environ.get("CXX", "c++")
    args = [
        cxx, "-std=c++17", "-O2", "-fPIC", "-shared",
        "-Wno-unused-but-set-variable", "-Wno-unused-variable", "-Wno-unused-parameter",
        f"-I{INCLUDE_DIR}",
        str(cpp_path),
        "-o", str(out_dylib),
    ]
    if is_macos:
        args += ["-Wl,-force_load," + str(libpf)]
    else:
        args += ["-Wl,--whole-archive", str(libpf), "-Wl,--no-whole-archive"]

    res = subprocess.run(args, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"compile failed for {cpp_path}:\n{res.stderr[-2000:]}")


def compile_one(bench_dir: Path) -> Path:
    """Codegen + compile one bench strategy. Returns path to built .dylib."""
    pine = bench_dir / "strategy.pine"
    if not pine.exists():
        raise FileNotFoundError(f"missing strategy.pine in {bench_dir}")
    out_dir = WORKDIR / bench_dir.name
    cpp = out_dir / "generated.cpp"
    dylib = out_dir / "strategy.dylib"
    transpile_pine(pine, cpp)
    compile_dylib(cpp, dylib)
    return dylib


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="Substring filter on bench slug")
    ap.add_argument("--clean", action="store_true",
                    help=f"Delete {WORKDIR.relative_to(REPO_ROOT)} before building")
    args = ap.parse_args()

    if args.clean and WORKDIR.exists():
        shutil.rmtree(WORKDIR)

    n_ok = n_fail = 0
    failed: list[tuple[str, str]] = []
    for d in sorted(STRATEGIES.iterdir()):
        if not d.is_dir() or d.name.startswith("_"):
            continue
        if args.only and args.only not in d.name:
            continue
        try:
            dylib = compile_one(d)
            print(f"  [{d.name:42s}] OK    {dylib.relative_to(REPO_ROOT)}")
            n_ok += 1
        except Exception as e:
            msg = str(e).splitlines()[0][:200]
            print(f"  [{d.name:42s}] FAIL  {msg}")
            failed.append((d.name, msg))
            n_fail += 1

    print(f"\nbuilt {n_ok}, failed {n_fail}")
    if failed:
        print("\nfailed:")
        for name, err in failed:
            print(f"  {name}: {err}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
