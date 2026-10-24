#!/usr/bin/env python3
"""Emit the checked runtime and independent native C API inventories as TSV."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
from types import ModuleType


COLUMNS = "symbol\tdeclared_in\timplemented_in_c_abi_cpp"
NATIVE_SECTION = (
    "# native C API: independent; implementations in src/native_c_host.cpp"
)
REPORT_MODES = (
    "# report modes: FULL only through the C ABI/CLI; SUMMARY_ONLY and STREAMED "
    "refuse typed (report_mode_unsupported)"
)


def _load_checker() -> ModuleType:
    path = Path(__file__).resolve().with_name("check_c_abi_runtime.py")
    spec = importlib.util.spec_from_file_location("check_c_abi_runtime", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load ABI checker from {path}")
    checker = importlib.util.module_from_spec(spec)
    # The default stdout-only invocation must not create a __pycache__.
    saved = sys.dont_write_bytecode
    try:
        sys.dont_write_bytecode = True
        spec.loader.exec_module(checker)
    finally:
        sys.dont_write_bytecode = saved
    return checker


def _render(checker: ModuleType) -> bytes:
    include_dir = checker.ROOT / "include" / "pineforge"
    declarations = [
        (symbol, header)
        for header in checker.RUNTIME_HEADERS
        for symbol in checker._pf_api_names(include_dir / header)
    ]
    implementations = checker._pf_api_names(checker.ROOT / "src" / "c_abi.cpp")
    implemented = set(implementations)
    native_header = include_dir / "native_c_api.h"
    native_declarations = checker._pf_api_names(native_header)
    native_implementations = checker._pf_api_names(
        checker.ROOT / "src" / "native_c_host.cpp"
    )

    # Assert measured lengths again, independently of main()'s current checks.
    measurements = (
        ("runtime_declarations", len(declarations),
         checker.EXPECTED_PUBLIC_DECLARATIONS),
        ("runtime_implementations", len(implementations),
         checker.EXPECTED_RUNTIME_IMPLEMENTATIONS),
        ("runtime rows marked yes",
         sum(symbol in implemented for symbol, _ in declarations),
         checker.EXPECTED_RUNTIME_IMPLEMENTATIONS),
        ("native_c_api_declarations", len(native_declarations),
         checker.EXPECTED_NATIVE_C_API_DECLARATIONS),
        ("native_c_api_implementations", len(native_implementations),
         checker.EXPECTED_NATIVE_C_API_IMPLEMENTATIONS),
    )
    for name, measured, expected in measurements:
        if measured != expected:
            raise ValueError(f"{name} measured {measured} != checker count {expected}")
    if (set(native_declarations) != checker.EXPECTED_NATIVE_C_API
            or set(native_implementations) != checker.EXPECTED_NATIVE_C_API):
        raise ValueError("native C API inventory differs from EXPECTED_NATIVE_C_API")

    lines = [COLUMNS]
    lines.extend(
        f"{symbol}\t{header}\t{'yes' if symbol in implemented else 'no'}"
        for symbol, header in sorted(declarations)
    )
    lines.extend((NATIVE_SECTION, COLUMNS))
    lines.extend(
        f"{symbol}\t{native_header.name}\t{'yes' if symbol in implemented else 'no'}"
        for symbol in sorted(native_declarations)
    )
    lines.extend((
        f"# native_c_api_declarations={len(native_declarations)} "
        f"native_c_api_implementations={len(native_implementations)}",
        REPORT_MODES,
        f"# runtime_declarations={len(declarations)} "
        f"runtime_implementations={len(implementations)} "
        f"native_c_api_declarations={len(native_declarations)}",
    ))
    body = ("\n".join(lines) + "\n").encode("utf-8")
    return body + f"# sha256={hashlib.sha256(body).hexdigest()}\n".encode("ascii")


def _write_atomic(output: Path, report: bytes, *, force: bool) -> None:
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", prefix=f".{output.name}.", suffix=".tmp",
            dir=output.parent, delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(report)
            stream.flush()
            os.fsync(stream.fileno())
        if force:
            os.replace(temporary, output)
        else:
            # Link/unlink is the portable no-clobber move: unlike POSIX rename,
            # it also refuses a destination created while the temp was written.
            try:
                os.link(temporary, output)
            except FileExistsError:
                raise FileExistsError(
                    f"{output} already exists; use --force to replace it"
                ) from None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, metavar="FILE",
                        help="write the inventory atomically instead of to stdout")
    parser.add_argument("--force", action="store_true",
                        help="allow replacing an existing --output file")
    args = parser.parse_args(argv)
    if args.force and args.output is None:
        parser.error("--force requires --output")

    try:
        checker = _load_checker()
        result = checker.main()
        if result != 0:
            return result
        report = _render(checker)
        if args.output is None:
            sys.stdout.buffer.write(report)
        else:
            _write_atomic(args.output, report, force=args.force)
    except (ImportError, OSError, ValueError) as exc:
        print(f"emit_abi_export_list: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
