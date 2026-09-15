#!/usr/bin/env python3
"""Compile-only proof of the complete public intent row mirror prefix.

The source parser supplies the frozen v9 field table; the generated TU then
asks the actual compiler to compare every field's offset and size with that
frozen type. No linked or produced executable is run.
"""
from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

from check_pending_order_prefix import FIXTURE, _header_from_fixture, _layout, _struct_fields

ROOT = Path(__file__).resolve().parents[1]


def check() -> None:
    frozen_fixture = FIXTURE.parent / "basev9"
    frozen_fields, frozen_size = _layout(_struct_fields(_header_from_fixture(
        frozen_fixture,
        "f2df706062ee0509c499b5c757a8e1ac83fccec6",
        "3ae04a5eec3f8eb58a05ac48f994a2c6d0ab9579")))
    current = _layout(_struct_fields((ROOT / "include/pineforge/pending_order_mirror.hpp").read_text()))
    if current != (frozen_fields, frozen_size):
        raise SystemExit("current public mirror differs from frozen v9 by source table")
    if len(frozen_fields) != 406 or frozen_size != 3192:
        raise SystemExit("frozen v9 public mirror contract is not 406 fields / 3192 bytes")

    assertions = [
        "static_assert(sizeof(pf_pending_order_v1_t) == 3192);",
        "static_assert(alignof(pf_pending_order_v1_t) == 8);",
        "static_assert(sizeof(frozen::pf_pending_order_v1_t) == 3192);",
        "static_assert(alignof(frozen::pf_pending_order_v1_t) == 8);",
    ]
    for name, _ctype, offset, size in frozen_fields:
        assertions.append(f"static_assert(offsetof(pf_pending_order_v1_t, {name}) == {offset});")
        assertions.append(f"static_assert(offsetof(pf_pending_order_v1_t, {name}) == offsetof(frozen::pf_pending_order_v1_t, {name}));")
        assertions.append(f"static_assert(sizeof(((pf_pending_order_v1_t*)0)->{name}) == sizeof(((frozen::pf_pending_order_v1_t*)0)->{name}));")
    # Load C integer typedefs globally before including the frozen POD in its
    # namespace; GCC's include guard would otherwise leave them only in frozen.
    source = "#include <stdint.h>\n#include <cstddef>\n#include <type_traits>\nnamespace frozen {\n#include \"frozen_pending_order_mirror.hpp\"\n}\n#include <pineforge/pending_order_mirror.hpp>\n" + "\n".join(assertions) + "\n"
    compiler = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory(prefix="pf-pending-prefix-compiler-") as directory:
        root = Path(directory)
        (root / "frozen_pending_order_mirror.hpp").write_text(_header_from_fixture(
            frozen_fixture,
            "f2df706062ee0509c499b5c757a8e1ac83fccec6",
            "3ae04a5eec3f8eb58a05ac48f994a2c6d0ab9579"))
        source_path = root / "proof.cpp"
        object_path = root / "proof.o"
        source_path.write_text(source)
        result = subprocess.run(
            [compiler, "-std=c++17", "-I", str(ROOT / "include"), "-I", str(root),
             "-c", str(source_path), "-o", str(object_path)],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise SystemExit("compiler mirror proof failed:\n" + result.stderr)
    print("compiler mirror proof: all 406 fields and 3192-byte v9 prefix match")


if __name__ == "__main__":
    check()
