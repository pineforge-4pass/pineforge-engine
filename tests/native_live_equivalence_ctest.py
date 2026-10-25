"""CTest entry point for the corpus equivalence driver and its prerequisites."""

import argparse
import ast
from pathlib import Path
import runpy
import sys
import tempfile


SKIP_CODE = 77
SKIP_REASON = "SKIP native_live_equivalence_e2e prerequisites absent: "


def probe_names(script):
    # Read the driver's own population without importing its receipt helper
    # before the missing-prerequisites check. Never keep a second probe list.
    for node in ast.parse(script.read_text()).body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == "PROBES"
                for target in node.targets):
            probes = ast.literal_eval(node.value)
            if isinstance(probes, dict) and probes and all(isinstance(key, str) for key in probes):
                return probes.keys()
            raise ValueError("the equivalence driver's PROBES must be a non-empty dictionary")
    raise ValueError("the equivalence driver has no PROBES declaration")


def missing_prerequisites(root, build, script):
    paths = [(str(build / relative), build / relative) for relative in (
        "bin/pineforge-live", "compile_commands.json", "lib/libpineforge.a")]
    sources = ["corpus/data/ohlcv_ETH-USDT-USDT_1m.csv", "tests/native_live_receipts.py"]
    sources.extend(f"corpus/validation/{probe}/generated.cpp" for probe in probe_names(script))
    paths.extend((relative, root / relative) for relative in sources)
    return [name for name, path in paths if not path.is_file()]


def run_driver(script, arguments):
    previous_argv, previous_path = sys.argv, sys.path[:]
    sys.argv = [str(script), *arguments]
    sys.path.insert(0, str(script.parent))
    try:
        try:
            # Execute the real __main__, with its full default probe/scenario
            # population. Its output and exceptions remain visible to CTest.
            runpy.run_path(str(script), run_name="__main__")
        except SystemExit as result:
            if result.code is None:
                code = 0
            elif isinstance(result.code, int):
                code = result.code
            else:
                print(result.code, file=sys.stderr)
                code = 1
        else:
            code = 0
    finally:
        sys.argv = previous_argv
        sys.path[:] = previous_path
    if code == SKIP_CODE:
        print("equivalence driver exited 77 after prerequisites passed; treating as failure",
              file=sys.stderr)
        return 1
    # Normalize every other non-zero status too: e.g. SystemExit(256) must
    # not wrap to shell exit 0 and turn a failed driver into a CTest pass.
    return 0 if code == 0 else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--libraries-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--sanitizer", choices=("asan", "tsan"))
    arguments = parser.parse_args()
    root, build, script = (path.resolve() for path in (
        arguments.root, arguments.build_dir, arguments.script))
    # Match the existing genuine-retry ctypes witness: Python starts with the
    # build's runtime preloaded, native children keep their sanitizer options,
    # and LSan checks after native cleanup rather than after CPython teardown.
    from native_live_sanitizer import finalize_sanitizer, prepare_sanitizer, run_with_finalizer
    prepare_sanitizer(arguments.sanitizer)

    def execute():
        missing = missing_prerequisites(root, build, script)
        if missing:
            print(SKIP_REASON + "; ".join(missing), flush=True)
            return SKIP_CODE
        output_root = arguments.out.resolve()
        output_root.mkdir(parents=True, exist_ok=True)
        retained = Path(tempfile.mkdtemp(prefix="run-", dir=output_root))
        libraries = arguments.libraries_dir.resolve() / retained.name
        output = retained / "results"
        print(f"Equivalence output: {output}; libraries: {libraries}", flush=True)
        return run_driver(script, [
            "--root", str(root), "--build-dir", str(build),
            "--libraries-dir", str(libraries), "--out", str(output)])

    return run_with_finalizer(execute, finalize_sanitizer)


if __name__ == "__main__":
    raise SystemExit(main())
