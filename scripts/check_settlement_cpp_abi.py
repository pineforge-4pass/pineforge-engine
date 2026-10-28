#!/usr/bin/env python3
"""Audit settlement ABI pairs and native-order namespace transitions.

The default mode consumes every historical receipt supplied by CMake and
links the actual host-fc7aad6 archive in both stale directions, without running
the callers. It audits the historical relocation manifests and live surface.
The public order controls additionally reject a copied-header v7 caller, or
load rebuilt report modules and compare every transcript byte with the exact
pre-bump Git tree. Neither order control requires private fixtures.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from check_aggregate_cpp_versions import clean
from cpp_abi_pairing import (
    CURRENT_EPOCH, PairingError, audit_prepared_receipt, enforce_receipt_mode,
    execute_frozen_pair, _compile, _link,
)
from prepare_settlement_cpp_abi_base import (
    copied_cache, current_eigen_source, extract_tar, identity, read_cache, run,
)

ROOT = Path(__file__).resolve().parents[1]
# The active transition, and the historical one whose retired surface and
# added hooks the live tree must still show.
MANIFEST = ROOT / "tests/fixtures/native_cpp_abi/host-fc7aad6/relocation-manifest-v18-v19.json"
V16_V18_MANIFEST = ROOT / "tests/fixtures/native_cpp_abi/host-ab9714b/relocation-manifest-v16-v18.json"
# The private-fixture PRs move both relocation manifests out of the public tree
# with the rest of tests/fixtures/native_cpp_abi/.  Absent, the checker skips
# with the project's pinned literal -- the one the private-fixture ctest rows
# print and tests/CMakeLists.txt matches with SKIP_REGULAR_EXPRESSION -- rather
# than failing on read_text(); present, every check below runs as before.
PRIVATE_FIXTURE_SKIP_LITERAL = "SKIP private fixtures not present: "
ORDER_NAMESPACE = "native_order_v8"
PREVIOUS_ORDER_NAMESPACE = "native_order_v7"
# Exact tree before the order namespace bump. Only local Git objects are used.
ORDER_BASE_COMMIT = "4dc08f4f7b74451ce78a2d73efadd6e8703e031b"
ORDER_BASE_TREE = "51abf4eb0b0a7c809c93b9ddd111bd6cb8b52e75"
REPORT_FIXTURE = "tests/test_native_settlement_row_finite.cpp"


def missing_private_fixtures() -> list:
    """The relocation manifests this checker reads that the tree lacks."""
    return [path for path in (MANIFEST, V16_V18_MANIFEST) if not path.exists()]
RETIRED_HEADER = "pineforge/source/pine_pending_intent.hpp"
_OLD = "legacy"
_RUN = _OLD + "_run_"
_STREAM = _OLD + "_stream_"
RETIRED_SEAMS = (
    _RUN + "simple", _RUN + "tf", _RUN + "rich",
    _STREAM + "begin", _STREAM + "push_bar", _STREAM + "push_tick",
    _STREAM + "push_ticks", _STREAM + "advance_time", _STREAM + "end",
    "validate_source_lifecycle", "preflight_source_lifecycle",
    "apply_source_pre_close_lifecycle", "apply_source_pending_removals",
)


def verify_order_epoch(include: Path) -> str:
    order = clean((include / "pineforge/native_order.hpp").read_text())
    epochs = re.findall(r"\binline\s+namespace\s+(native_order_v\d+)\s*\{", order)
    if epochs != [ORDER_NAMESPACE]:
        raise RuntimeError("current native_order.hpp must declare only " + ORDER_NAMESPACE)
    return ORDER_NAMESPACE


def verify(include: Path) -> dict:
    engine = (include / "pineforge/engine.hpp").read_text()
    native = (include / "pineforge/native_host.hpp").read_text()
    manifest = json.loads(MANIFEST.read_text())
    if manifest.get("schema") != "pineforge-epoch-relocation/v1":
        raise RuntimeError("unexpected v18-v19 relocation schema")
    if manifest.get("transition") != {
            "from": "engine_script_run_v18", "to": "engine_script_run_v19"}:
        raise RuntimeError("v18-v19 relocation transition drift")
    if "inline namespace " + CURRENT_EPOCH not in engine:
        raise RuntimeError("current engine epoch is not v19")
    capability = manifest.get("hostCapability") or {}
    if capability != {"from": "PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V18",
                      "to": "PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19"}:
        raise RuntimeError("v18-v19 host capability transition drift")
    if "#define " + capability["to"] + " 1" not in native:
        raise RuntimeError("current native host omits its v19 capability macro")
    if capability["from"] in native:
        raise RuntimeError("current native host still defines the v18 capability macro")
    # Keep the historical v18/v19 relocation authenticated as it was recorded.
    # The live order value epoch has since advanced independently of the host.
    current_order_epoch = verify_order_epoch(include)
    order_epoch = manifest.get("orderEpoch") or {}
    if order_epoch != {"from": "native_order_v6", "to": "native_order_v7"}:
        raise RuntimeError("v18-v19 relocation order epoch drift")
    historical = json.loads(V16_V18_MANIFEST.read_text())
    if historical.get("schema") != "pineforge-r4-d-relocation/v1" or historical.get(
            "transition") != {"from": "engine_script_run_v16", "to": "engine_script_run_v18"}:
        raise RuntimeError("historical v16-v18 relocation manifest drift")
    if (include / RETIRED_HEADER).exists():
        raise RuntimeError("retired source order header remains installed")
    present = [name for name in RETIRED_SEAMS if name in engine]
    if present:
        raise RuntimeError("retired engine seams remain: " + ", ".join(present))
    required_virtuals = {
        "prepare_native_begin", "on_native_bar_open", "on_native_input",
        "on_native_tick", "on_native_timeframe_bar", "on_native_margin_call",
        "on_native_recalculate", "on_native_sub_bar",
    }
    # Policy hooks answer a value, so their declaration is not "virtual void".
    required_answering_virtuals = {
        "resolve_margin_requirement", "margin_check_allowed",
        "resolve_anchored_level",
    }
    declared = set(historical.get("addedVirtuals", []))
    if not (required_virtuals | required_answering_virtuals).issubset(declared):
        raise RuntimeError("relocation manifest omits a native hook")
    if not all("virtual void " + name in native for name in required_virtuals):
        raise RuntimeError("current native host omits a required hook")
    if not all(re.search(r"virtual\s+[^;{]+?\b" + name + r"\s*\(", native)
               for name in required_answering_virtuals):
        raise RuntimeError("current native host omits a required policy hook")
    # N5: the generic host hash extension is a BacktestEngine virtual, so the
    # engine header carries it, beside the deprecated spelling it forwards to.
    required_engine_virtuals = {"hash_host_extension"}
    if not required_engine_virtuals.issubset(declared):
        raise RuntimeError("relocation manifest omits an engine hook")
    if not all("virtual void " + name + "(BrokerStateHashSink&) const;" in engine
               for name in required_engine_virtuals | {"hash_source_extension"}):
        raise RuntimeError("current engine omits a required hash extension hook")
    for key in ("addedVirtuals", "removedVirtuals"):
        if manifest.get(key) != []:
            raise RuntimeError("v18-v19 relocation " + key + " drift: v19 moves no host hook")
    pairs = manifest.get("rejectionPairs")
    if pairs != [["v18-frozen", "v19-current"], ["v19-current", "v18-frozen"]]:
        raise RuntimeError("v18/v19 rejection pairs drift")
    return {"transition": manifest["transition"], "rejectionPairs": pairs,
            "orderEpoch": order_epoch,
            "currentOrderEpoch": current_order_epoch,
            "addedStorage": manifest.get("addedStorage", []),
            "retiredHeader": RETIRED_HEADER}


def require_order_rejection(exit_code: int, diagnostic: str) -> None:
    if exit_code == 0:
        raise PairingError("v7 order caller unexpectedly linked against the live archive")
    if exit_code < 0:
        raise PairingError("order caller linker terminated by a signal")
    witness = PREVIOUS_ORDER_NAMESPACE + "::WorkingRequestCore::WorkingRequestCore"
    darwin_symbols = False
    for line in diagnostic.splitlines():
        if line.startswith("Undefined symbols for architecture "):
            darwin_symbols = True
        if witness not in line:
            continue
        if re.search(r"undefined reference|undefined symbol|unresolved external",
                     line, re.IGNORECASE):
            return
        if darwin_symbols and line.lstrip().startswith('"') and "referenced from:" in line:
            return
    raise PairingError("order caller rejected for the wrong reason; missing unresolved "
                       + witness + ":\n" + diagnostic)


def verify_order_rejection(args: argparse.Namespace, root: Path) -> dict:
    """Use the existing caller compiler and archive linker with copied v7 headers."""
    source = '''#include <pineforge/native_order.hpp>
int main() {
    pineforge::native_order::WorkingRequestCore core({"order-abi-caller", 1});
    return static_cast<int>(core.history_end());
}
'''
    flags = list(args.extra_flag)
    current_object, current_compile = _compile(
        args.compiler, flags, "order_current", source, args.include,
        args.generated_include, root)
    positive = _link(args.compiler, flags, "order_current_accept", current_object,
                     args.library, "accept", ORDER_NAMESPACE, ORDER_NAMESPACE, root)
    copied_include = root / "v7-include"
    shutil.copytree(args.include, copied_include)
    for header in copied_include.rglob("*"):
        if header.is_file():
            text = header.read_text()
            if ORDER_NAMESPACE in text:
                header.write_text(text.replace(ORDER_NAMESPACE, PREVIOUS_ORDER_NAMESPACE))
    stale_object, stale_compile = _compile(
        args.compiler, flags, "order_v7", source, copied_include,
        args.generated_include, root)
    argv = [args.compiler, "-std=c++17", *flags, str(stale_object), str(args.library),
            "-pthread", "-o", str(root / "order_v7_reject")]
    result = subprocess.run(argv, text=True, capture_output=True)
    diagnostic = result.stdout + result.stderr
    (root / "order_v7_reject.link.log").write_text(diagnostic)
    require_order_rejection(result.returncode, diagnostic)
    verify_order_epoch(args.include)
    return {"compiles": [current_compile, stale_compile],
            "positiveLink": positive,
            "staleLink": {"argv": argv, "exitCode": result.returncode,
                          "diagnostic": diagnostic, "outcome": "expected-rejection"},
            "executedBinaries": 0}


def prepare_order_baseline(current_build: Path, output: Path, jobs: int) -> dict:
    """Reuse historical-provider extraction, cache copying and offline dependency setup."""
    cache = read_cache(current_build / "CMakeCache.txt")
    if Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != ROOT:
        raise PairingError("order baseline cache belongs to a different source repository")
    commit = run(["git", "-C", str(ROOT), "rev-parse",
                  ORDER_BASE_COMMIT + "^{commit}"]).decode().strip()
    tree = run(["git", "-C", str(ROOT), "rev-parse",
                ORDER_BASE_COMMIT + "^{tree}"]).decode().strip()
    if (commit, tree) != (ORDER_BASE_COMMIT, ORDER_BASE_TREE):
        raise PairingError("pre-bump order provider commit/tree mismatch")
    # Keep the frozen build's whole tracked input tree, including data files
    # included from outside src/ (metrics_annualization.hpp reads docker/).
    # Only the corpus gitlink and the unused benchmark/documentation trees
    # are omitted; do not guess a source/header dependency whitelist.
    source_paths = run(["git", "-C", str(ROOT), "ls-tree", "-z", "--name-only",
                        ORDER_BASE_COMMIT]).decode().split("\0")
    source_paths = [path for path in source_paths
                    if path and path not in {"corpus", "benchmarks", "docs"}]
    source_raw = run(["git", "-C", str(ROOT), "archive", "--format=tar",
                      ORDER_BASE_COMMIT, *source_paths])
    output.mkdir()
    (output / "source.tar").write_bytes(source_raw)
    source, build = output / "source", output / "build"
    extract_tar(source_raw, source)
    if (ROOT / REPORT_FIXTURE).read_bytes() != (source / REPORT_FIXTURE).read_bytes():
        raise PairingError("report controls must remain identical to the frozen base fixture")
    settings = copied_cache(cache)
    eigen = current_eigen_source(cache, current_build)
    if eigen is not None:
        settings["FETCHCONTENT_SOURCE_DIR_EIGEN"] = str(eigen)
    settings.update({
        "PINEFORGE_BUILD_SOURCE_LAYER": cache.get("PINEFORGE_BUILD_SOURCE_LAYER", "ON"),
        "PINEFORGE_BUILD_TESTS": "OFF", "PINEFORGE_BUILD_TUTORIAL": "OFF",
        "PINEFORGE_BUILD_CORPUS_STRATEGIES": "OFF", "PINEFORGE_BUILD_LIVE_RUNNER": "OFF",
        "PINEFORGE_BUILD_BENCH_STRATEGIES": "OFF", "PINEFORGE_BUILD_SPEED_BENCH": "OFF",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON", "FETCHCONTENT_FULLY_DISCONNECTED": "ON",
    })
    configure = ["cmake", "-S", str(source), "-B", str(build), "-G",
                 cache["CMAKE_GENERATOR"], *[f"-D{key}={value}" for key, value in settings.items()]]
    run(configure, log=output / "configure.log")
    build_argv = ["cmake", "--build", str(build), "--target", "pineforge",
                  "--parallel", str(jobs)]
    run(build_argv, log=output / "build.log")
    archive = build / "lib/libpineforge.a"
    if not archive.read_bytes().startswith(b"!<arch>\n"):
        raise PairingError("pre-bump order provider is not a static archive")
    receipt = {"commit": commit, "tree": tree, "archive": str(archive),
               "archiveSha256": identity(archive)["sha256"],
               "sourceArchive": identity(output / "source.tar"),
               "sourcePaths": source_paths,
               "fixture": identity(source / REPORT_FIXTURE),
               "settings": settings, "configureArgv": configure, "buildArgv": build_argv,
               "networkFetches": 0}
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    return receipt


def order_module_source() -> str:
    # Extend the existing exact-word report controls as a dynamically loaded module.
    return '''#include <pineforge/native_order.hpp>
#include <type_traits>
#ifdef PINEFORGE_REQUIRE_NATIVE_ORDER_V8
static_assert(std::is_same<pineforge::native_order::Request,
    pineforge::native_order::native_order_v8::Request>::value,
    "rebuilt module must use the v8 order values");
#endif
#define main order_report_controls_main
#include "test_native_settlement_row_finite.cpp"
#undef main
extern "C" __attribute__((visibility("default"))) int pineforge_order_report_controls() {
    pineforge::native_order::WorkingRequestCore core({"order-module-boundary", 1});
    if (core.history_end() != 0 || core.live_size() != 0) return 1;
    char name[] = "order-report-controls";
    char controls[] = "--controls";
    char* argv[] = {name, controls, nullptr};
    return order_report_controls_main(2, argv);
}
'''


def require_report_transcript(raw: bytes) -> None:
    """Both empty output and a success banner without report payloads must fail."""
    required = ("finite_closed_zero", "finite_closed_owned", "finite_range_zero",
                "finite_range_owned", "kernel_closed_normal", "kernel_range_normal")
    lines = [line.split() for line in raw.decode("ascii").splitlines()]
    for name in required:
        rows = [line for line in lines if line and line[0] == name]
        if (len(rows) != 1 or len(rows[0]) < 2
                or not all(re.fullmatch(r"[0-9a-f]{16}", word) for word in rows[0][1:])):
            raise PairingError("missing exact-word report transcript for " + name)


def verify_order_rebuild(args: argparse.Namespace, root: Path) -> dict:
    module_source = order_module_source()
    flags = list(args.extra_flag)
    current_object, current_compile = _compile(
        args.compiler, [*flags, "-fPIC", "-DPINEFORGE_REQUIRE_NATIVE_ORDER_V8",
                        "-I", str(ROOT / "tests")],
        "order_rebuilt", module_source, args.include, args.generated_include, root)
    verify_order_epoch(args.include)
    baseline = prepare_order_baseline(args.current_build, root / "baseline", args.jobs)
    frozen_source = root / "baseline/source"
    frozen_build = root / "baseline/build"
    frozen_object, frozen_compile = _compile(
        args.compiler, [*flags, "-fPIC", "-I", str(frozen_source / "tests")],
        "order_before", module_source, frozen_source / "include",
        frozen_build / "include", root)
    loader_source = r'''#include <dlfcn.h>
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    dlerror();
    auto entry = reinterpret_cast<int (*)()>(dlsym(module, "pineforge_order_report_controls"));
    const char* error = dlerror();
    if (error || !entry) {
        std::fprintf(stderr, "dlsym: %s\n", error ? error : "missing entry");
        dlclose(module);
        return 1;
    }
    const int result = entry();
    return dlclose(module) == 0 ? result : 1;
}
'''
    loader_object, loader_compile = _compile(
        args.compiler, flags, "order_loader", loader_source, args.include,
        args.generated_include, root)
    loader = root / "order_loader"
    run([args.compiler, *flags, str(loader_object), "-pthread",
         *([] if sys.platform == "darwin" else ["-ldl"]), "-o", str(loader)],
        log=root / "order_loader.link.log")
    transcripts = {}
    for name, object_file, library in (
            ("before", frozen_object, Path(baseline["archive"])),
            ("rebuilt", current_object, args.library)):
        module = root / (name + ".so")
        shared_flags = (["-dynamiclib", "-Wl,-undefined,error"] if sys.platform == "darwin"
                        else ["-shared", "-Wl,-z,defs"])
        run([args.compiler, *flags, *shared_flags, str(object_file), str(library),
             "-pthread", "-o", str(module)], log=root / (name + ".link.log"))
        # Separate loader processes prevent cross-module symbol interposition.
        raw = run([str(loader), str(module)], log=root / (name + ".run.log"))
        (root / (name + ".transcript")).write_bytes(raw)
        require_report_transcript(raw)
        transcripts[name] = raw
    if transcripts["before"] != transcripts["rebuilt"]:
        raise PairingError("rebuilt module changed trade/report bytes; compare "
                           + str(root / "before.transcript") + " and "
                           + str(root / "rebuilt.transcript"))
    return {"baseline": baseline,
            "compiles": [current_compile, frozen_compile, loader_compile],
            "transcript": identity(root / "before.transcript"),
            "comparison": "all exact-word trade/report transcript bytes identical",
            "executedBinaries": len(transcripts)}


def verify_pair(args: argparse.Namespace) -> dict:
    surface = verify(args.include)
    inputs = [
        audit_prepared_receipt(args.base_receipt, "R2 base"),
        audit_prepared_receipt(args.prior_receipt, "R3 prior"),
        audit_prepared_receipt(args.v13_receipt, "native v13"),
        audit_prepared_receipt(args.v14_receipt, "native v14"),
        audit_prepared_receipt(args.v15_frozen_receipt, "frozen v15"),
        audit_prepared_receipt(args.v16_frozen_receipt, "frozen v16"),
        audit_prepared_receipt(args.v18_frozen_receipt, "frozen v18"),
    ]
    pair = execute_frozen_pair(
        compiler=args.compiler,
        extra_flags=args.extra_flag,
        current_library=args.library,
        current_include=args.include,
        generated_include=args.generated_include,
        frozen_receipt=args.v18_frozen_receipt,
        kind="native",
        artifact_directory=args.receipt.parent,
    )
    return {
        "schemaVersion": "pineforge-settlement-abi/v2",
        "v18V19Surface": surface,
        "historicalInputs": inputs,
        "v18V19Pair": pair,
        "summary": {
            "compiled": len(pair["compiles"]),
            "linked": sum(row["outcome"] == "linked" for row in pair["links"]),
            "rejected": sum(row["outcome"] == "expected-rejection" for row in pair["links"]),
            "executedBinaries": 0,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--include", type=Path, required=True)
    parser.add_argument("--generated-include", type=Path, required=True)
    parser.add_argument("--base-receipt", type=Path)
    parser.add_argument("--prior-receipt", type=Path)
    parser.add_argument("--v13-receipt", type=Path)
    parser.add_argument("--v14-receipt", type=Path)
    parser.add_argument("--v15-frozen-receipt", type=Path)
    parser.add_argument("--v16-frozen-receipt", type=Path)
    parser.add_argument("--v18-frozen-receipt", type=Path)
    parser.add_argument("--order-check", choices=("reject", "rebuild"))
    parser.add_argument("--current-build", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--receipt", type=Path, required=True)
    receipt_mode = parser.add_mutually_exclusive_group()
    receipt_mode.add_argument("--skip-if-receipt-missing", action="store_true")
    receipt_mode.add_argument("--require-receipts", action="store_true")
    args = parser.parse_args()
    if args.order_check:
        if args.order_check == "rebuild" and args.current_build is None:
            parser.error("--order-check rebuild requires --current-build")
        if not 1 <= args.jobs <= 64:
            parser.error("--jobs must be 1..64")
        args.include = args.include.resolve()
        args.library = args.library.resolve()
        args.generated_include = args.generated_include.resolve()
        args.receipt = args.receipt.resolve()
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix="order-" + args.order_check + "-",
                                     dir=args.receipt.parent))
        try:
            check = verify_order_rejection if args.order_check == "reject" else verify_order_rebuild
            result = check(args, root)
        except RuntimeError as error:
            raise SystemExit("native order C++ ABI: " + str(error)) from error
        result.update({"orderEpoch": ORDER_NAMESPACE, "artifactsDirectory": str(root)})
        args.receipt.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        print("native order C++ ABI: " + args.order_check + " verified; artifacts " + str(root))
        return 0
    for name in ("base_receipt", "prior_receipt", "v13_receipt", "v14_receipt",
                 "v15_frozen_receipt", "v16_frozen_receipt", "v18_frozen_receipt"):
        if getattr(args, name) is None:
            parser.error("--" + name.replace("_", "-") + " is required for settlement ABI pairs")
    try:
        absent = missing_private_fixtures()
        if absent:
            print(PRIVATE_FIXTURE_SKIP_LITERAL + str(absent[0].relative_to(ROOT)))
            return 77
        mode = enforce_receipt_mode(
            (args.base_receipt, args.prior_receipt, args.v13_receipt,
             args.v14_receipt, args.v15_frozen_receipt, args.v16_frozen_receipt,
             args.v18_frozen_receipt),
            skip=args.skip_if_receipt_missing, require=args.require_receipts,
            label="settlement C++ ABI")
        if mode is not None:
            return mode
        result = verify_pair(args)
    except PairingError as error:
        raise SystemExit("settlement C++ ABI: " + str(error))
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    summary = result["summary"]
    print(f"settlement C++ ABI: {summary['compiled']} callers compiled; "
          f"{summary['linked']} positive links; {summary['rejected']} v18/v19 rejections; "
          "no executable run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
