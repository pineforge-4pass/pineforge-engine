#!/usr/bin/env python3
"""Unit controls for check_native_include_independence.py; no CMake/compiler run."""
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from check_native_include_independence import (
    NATIVE_EXAMPLES,
    ROOT,
    Finding,
    archive_text,
    c_compile_command_flags,
    compile_command_flags,
    example_sources_on_disk,
    is_c_source,
    forbidden_dependency_entries,
    forbidden_symbol_lines,
    independence_exit_code,
    parse_depfile,
    remove_forbidden_prefix_trees,
    sanitize_compile_flags,
)


class NativeIncludeIndependenceTooling(unittest.TestCase):
    def test_archive_text_writes_only_when_evidence_is_requested(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "evidence"
            archive_text(evidence, Path("dependencies/native_host.hpp.d"), "header.d\n")
            self.assertEqual((evidence / "dependencies/native_host.hpp.d").read_text(), "header.d\n")
            archive_text(None, Path("ignored"), "ignored\n")
            self.assertFalse((evidence / "ignored").exists())

    def test_remove_forbidden_prefix_trees_keeps_generic_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary) / "prefix"
            base = prefix / "include/pineforge"
            (base / "source").mkdir(parents=True)
            (base / "source/pine.hpp").write_text("source\n")
            (base / "compat/pine").mkdir(parents=True)
            (base / "compat/pine/legacy.hpp").write_text("pine\n")
            (base / "native_host.hpp").write_text("generic\n")
            removed = remove_forbidden_prefix_trees(prefix)
            self.assertEqual({path.relative_to(base) for path in removed},
                             {Path("source"), Path("compat/pine")})
            self.assertFalse((base / "source").exists())
            self.assertFalse((base / "compat/pine").exists())
            self.assertEqual((base / "native_host.hpp").read_text(), "generic\n")

    def test_compile_command_sanitization_removes_all_include_and_output_paths(self):
        arguments = [
            "/tool/c++", "-O3", "-DNDEBUG", "-std=c++17", "-arch", "arm64",
            "-I", "/repo/include", "-I/build/include", "-isystem", "/sdk/include",
            "-iquote", "/repo/private", "-include", "/repo/pch.hpp", "-c",
            "/repo/runner/examples/native_market_strategy.cpp", "-o", "/build/example.o",
            "-MMD", "-MF", "/build/example.d", "-ffp-contract=off",
        ]
        flags = sanitize_compile_flags(
            arguments, source_file="/repo/runner/examples/native_market_strategy.cpp", compiler="/tool/c++")
        self.assertEqual(flags, ["-O3", "-DNDEBUG", "-std=c++17", "-arch", "arm64",
                                 "-ffp-contract=off"])
        self.assertFalse(any(flag.startswith(("-I", "-isystem", "-iquote")) for flag in flags))

    def test_cache_fallback_uses_active_configuration_and_cxx17(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            cache = {
                "CMAKE_CXX_COMPILER": "/tool/c++",
                "CMAKE_BUILD_TYPE": "Release",
                "CMAKE_CXX_FLAGS": "-Wall -I /repo/include",
                "CMAKE_CXX_FLAGS_RELEASE": "-O3 -ffp-contract=off",
            }
            compiler, flags, origin = compile_command_flags(build, cache)
            self.assertEqual(compiler, "/tool/c++")
            self.assertEqual(origin, "CMakeCache.txt")
            self.assertIn("-Wall", flags)
            self.assertIn("-O3", flags)
            self.assertIn("-ffp-contract=off", flags)
            self.assertIn("-std=c++17", flags)
            self.assertNotIn("-I", flags)
            self.assertNotIn("/repo/include", flags)

    def test_c_example_compiles_with_the_c_toolchain_not_cxx17(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            cache = {
                "CMAKE_C_COMPILER": "/tool/cc",
                "CMAKE_CXX_COMPILER": "/tool/c++",
                "CMAKE_BUILD_TYPE": "Release",
                "CMAKE_C_FLAGS": "-Wall -I /repo/include",
                "CMAKE_C_FLAGS_RELEASE": "-O2 -ffp-contract=off",
                "CMAKE_CXX_FLAGS_RELEASE": "-O3",
            }
            compiler, flags, origin = c_compile_command_flags(build, cache)
            self.assertEqual(compiler, "/tool/cc")
            self.assertEqual(origin, "CMakeCache.txt")
            self.assertIn("-O2", flags)
            self.assertNotIn("-O3", flags)
            self.assertIn("-std=c11", flags)
            self.assertNotIn("-std=c++17", flags)
            self.assertNotIn("-I", flags)
        self.assertTrue(is_c_source("examples/native/hello_kernel_c.c"))
        self.assertFalse(is_c_source("examples/native/hello_kernel.cpp"))
        # A .c operand is dropped from a reused compile command like a .cpp one.
        flags = sanitize_compile_flags(
            ["/tool/cc", "-O2", "-c", "/repo/examples/native/hello_kernel_c.c", "-o", "/b/x.o"],
            source_file="/repo/examples/native/hello_kernel_c.c", compiler="/tool/cc")
        self.assertEqual(flags, ["-O2"])

    def test_manifest_covers_every_example_source_on_disk(self):
        # The lane's own acceptance: the checker covers ALL example sources,
        # the C twin included. A source added to examples/native/ without a
        # manifest row fails here before it fails in the checker itself.
        listed = sorted(relative for _, relative in NATIVE_EXAMPLES)
        self.assertEqual(listed, sorted(example_sources_on_disk(ROOT)))
        self.assertIn("examples/native/hello_kernel_c.c", listed)
        self.assertTrue(all((ROOT / relative).is_file() for relative in listed))

    def test_depfile_and_nm_detection_preserve_offending_lines(self):
        with tempfile.TemporaryDirectory() as temporary:
            depfile = Path(temporary) / "headers.d"
            depfile.write_text("out.o: /prefix/include/pineforge/native_host.hpp \\\n+ /prefix/include/pineforge/source/pine.hpp \\\n+ /prefix/include/pineforge/compat/pine/legacy.hpp\n")
            found = forbidden_dependency_entries(parse_depfile(depfile))
            self.assertEqual(found, ["/prefix/include/pineforge/source/pine.hpp",
                                     "/prefix/include/pineforge/compat/pine/legacy.hpp"])
        symbols = "U pineforge::source::PineStrategyHost::run()\nU compat::pine::CapAttachment::x()\n"
        self.assertEqual(forbidden_symbol_lines(symbols), symbols.splitlines())

    def test_no_source_symbol_is_whitelisted(self):
        # The rich begin bridge takes `const void*`, so even the override
        # pointer no longer names the source layer anywhere in the kernel.
        neutral = ("U pineforge::engine_script_run_v18::BacktestEngine::run_rich("
                   "pineforge::Bar const*, void const*)")
        self.assertEqual(forbidden_symbol_lines(neutral), [])
        self.assertEqual(forbidden_symbol_lines(
            "U pineforge::source::PineStrategyHost::run()"),
            ["U pineforge::source::PineStrategyHost::run()"])
        overrides = ("U pineforge::engine_script_run_v18::BacktestEngine::run_rich("
                     "pineforge::Bar const*, pineforge::source::StrategyOverrides const*)")
        self.assertEqual(forbidden_symbol_lines(overrides), [overrides])

    def test_expect_fail_only_inverts_real_findings(self):
        finding = Finding("dependency", "header", "/prefix/include/pineforge/compat/pine/x.hpp")
        self.assertEqual(independence_exit_code([], expect_fail=False), 0)
        self.assertEqual(independence_exit_code([finding], expect_fail=False), 1)
        self.assertEqual(independence_exit_code([finding], expect_fail=True), 0)
        self.assertEqual(independence_exit_code([], expect_fail=True), 1)


if __name__ == "__main__":
    unittest.main()
