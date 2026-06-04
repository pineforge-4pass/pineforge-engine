#!/usr/bin/env python3
"""Accurate line-coverage via per-binary union (clang/llvm-cov only).

WHY THIS EXISTS
---------------
`llvm-cov report` over MANY test binaries that share inline/template code
(our header-only Pine wrappers: series.hpp, generic_matrix.hpp, ta.hpp, the
inline accessors in engine.hpp) emits "N functions have mismatched data" and
UNDERCOUNTS those headers. Each binary instantiates the same inline/template
function with its own coverage-mapping hash; the merged profile splinters per
hash, so a single cross-binary report only matches one variant and drops the
rest.

This helper sidesteps that: it asks `llvm-cov show` for each binary
individually (where every function matches its own binary's hash, so the
per-line counts are authoritative), then UNIONS line hits across binaries — a
line is executable if any binary maps it, covered if any binary executed it.
For headers it recovers the undercount.

METRIC NOTE: this counts PHYSICAL source lines (what `llvm-cov show` marks
executable), which differs from `llvm-cov report`/`export` — those count
region/expansion lines and yield larger absolute line totals. So this tool's
absolute %/counts are NOT directly comparable to totals.txt; use it to check the
RELATIVE truth for template-heavy headers (e.g. generic_matrix.hpp), where the
multi-binary `report` undercounts. The canonical headline metric remains
`llvm-cov report` (scripts/coverage.sh → totals.txt).

USAGE
-----
  cov_union.py --profdata P --bindir D --bin-glob 'test_*' SOURCE [SOURCE...]

Outputs a per-file line-coverage table + TOTAL to stdout. Exit 0 always
(measurement tool). With --uncovered FILE, also writes a file sorted ascending
by line coverage.
"""
from __future__ import annotations
import argparse, glob, os, re, subprocess, sys

LINE_RE = re.compile(r"^\s*(\d+)\|([^|]*)\|")
BANNER_RE = re.compile(r"^(/.*):$")


def find_llvm_cov() -> list[str]:
    if sys.platform == "darwin":
        from shutil import which
        if which("xcrun"):
            return ["xcrun", "llvm-cov"]
    return ["llvm-cov"]


def show_one(llvm_cov, binary, profdata, src):
    """Run `llvm-cov show` for ONE source file and return {lineno: covered_bool}
    for executable lines, deduped across instantiation groups (covered if ANY
    instantiation executed the line).

    Must be ONE file per invocation: passing many files to a single `show`
    suppresses template instantiation groups, which silently drops most of a
    template header's covered lines (e.g. generic_matrix.hpp).
    """
    r = subprocess.run(
        llvm_cov + ["show", binary, "-instr-profile", profdata, "--format=text", src],
        capture_output=True, text=True)
    out: dict[int, bool] = {}
    for ln in r.stdout.splitlines():
        m = LINE_RE.match(ln)
        if not m:
            continue
        cnt = m.group(2).strip()
        if cnt == "":
            continue  # non-executable line
        lineno = int(m.group(1))
        covered = cnt != "0"  # any non-zero (incl. human "1.2k") => covered
        prev = out.get(lineno)
        out[lineno] = covered if prev is None else (prev or covered)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profdata", required=True)
    ap.add_argument("--bindir", required=True, help="dir containing test binaries")
    ap.add_argument("--bin-glob", default="test_*")
    ap.add_argument("--uncovered", help="write file sorted ascending by line %%")
    ap.add_argument("sources", nargs="+")
    args = ap.parse_args()

    llvm_cov = find_llvm_cov()
    bins = sorted(b for b in glob.glob(os.path.join(args.bindir, args.bin_glob))
                  if os.path.isfile(b) and os.access(b, os.X_OK))
    if not bins:
        print(f"cov_union: no binaries matched {args.bindir}/{args.bin_glob}", file=sys.stderr)
        return 0

    # canonical source list (realpath -> display path)
    disp = {}
    for s in args.sources:
        if os.path.isfile(s):
            disp[os.path.realpath(s)] = s
    if not disp:
        print("cov_union: no source files found", file=sys.stderr)
        return 0

    union_exec: dict[str, set] = {ap_: set() for ap_ in disp}
    union_cov: dict[str, set] = {ap_: set() for ap_ in disp}
    # One `show` per (binary, file): multi-file show suppresses template
    # instantiation groups and undercounts template headers.
    for binary in bins:
        for ap_, d in disp.items():
            lines = show_one(llvm_cov, binary, args.profdata, d)
            for lineno, cov in lines.items():
                union_exec[ap_].add(lineno)
                if cov:
                    union_cov[ap_].add(lineno)

    rows = []
    tot_c = tot_n = 0
    for ap_, d in disp.items():
        n = len(union_exec[ap_]); c = len(union_cov[ap_])
        tot_c += c; tot_n += n
        rows.append((d, c, n, (100.0 * c / n) if n else 0.0))
    rows.sort(key=lambda r: r[0])

    w = max((len(r[0]) for r in rows), default=20)
    print(f"{'Filename':<{w}}  {'Lines':>7}  {'Missed':>7}  {'Cover':>7}")
    print("-" * (w + 26))
    for d, c, n, pct in rows:
        print(f"{d:<{w}}  {n:>7}  {n-c:>7}  {pct:>6.2f}%")
    print("-" * (w + 26))
    tot_pct = (100.0 * tot_c / tot_n) if tot_n else 0.0
    print(f"{'TOTAL':<{w}}  {tot_n:>7}  {tot_n-tot_c:>7}  {tot_pct:>6.2f}%")
    print(f"\n(union of {len(bins)} binaries — accurate header line coverage; "
          f"see scripts/cov_union.py header for why)")

    if args.uncovered:
        with open(args.uncovered, "w") as f:
            for d, c, n, pct in sorted(rows, key=lambda r: r[3]):
                f.write(f"{d}\t{n}\t{n-c}\t{pct:.2f}%\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
