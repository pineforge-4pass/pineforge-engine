#!/usr/bin/env python3
"""Mutation controls for the aggregate v19 ownership and ABI pair guards."""
from __future__ import annotations

from pathlib import Path
import shutil
import tempfile
import unittest

import check_aggregate_cpp_versions as checker
from cpp_abi_pairing import run_synthetic_pair


ROOT = Path(__file__).resolve().parents[1]


class AggregateVersions(unittest.TestCase):
    def copied_root(self) -> Path:
        directory = tempfile.TemporaryDirectory(prefix="pf-aggregate-versions-")
        self.addCleanup(directory.cleanup)
        root = Path(directory.name) / "repo"
        # __pycache__ / *.pyc: a Python guard CTest runs in parallel writes its
        # bytecode through a temporary file that can vanish between copytree's
        # listing and its copy; bytecode is never source input to the clone.
        shutil.copytree(ROOT, root, ignore=shutil.ignore_patterns(
            "build*", ".git", "corpus", "*.so", "*.a", ".native-fx-introduced-*",
            "__pycache__", "*.pyc"))
        return root

    def test_current_tree(self) -> None:
        checker.check(ROOT)

    def test_epoch_mutation_rejects(self) -> None:
        root = self.copied_root()
        path = root / "include/pineforge/engine.hpp"
        path.write_text(path.read_text().replace("engine_script_run_v19", "engine_script_run_v0", 1))
        with self.assertRaises(ValueError):
            checker.check(root)

    def test_stream_fold_mutation_rejects(self) -> None:
        root = self.copied_root()
        path = root / "src/engine_stream.cpp"
        path.write_text(path.read_text().replace(
            "integer(19); integer(broker_state_hash());",
            "if (false) { integer(19); integer(broker_state_hash()); }", 1))
        with self.assertRaises(ValueError):
            checker.check(root)

    def test_host_hash_seam_mutations_reject(self) -> None:
        # N5: the generic host seam of the broker-state fold. Each row breaks
        # one half of it: the projection's call, the default's forward, the
        # established marker, the public sink, the source host's use of it.
        for relative, before, after in (
            ("src/engine_state_hash.cpp",
             "    hash_host_extension(f);\n    return f.h;",
             "    hash_source_extension(f);\n    return f.h;"),
            ("src/engine_state_hash.cpp",
             "    hash_source_extension(sink);\n", "    sink.s(\"source:none\");\n"),
            ("src/engine_state_hash.cpp",
             "sink.s(\"source:none\");", "sink.s(\"host:none\");"),
            ("include/pineforge/engine.hpp",
             "virtual void hash_host_extension(BrokerStateHashSink&) const;",
             "void hash_host_extension(BrokerStateHashSink&) const;"),
            ("include/pineforge/engine.hpp",
             "class BrokerStateHashSink {", "class BrokerStateHashSinkMoved {"),
            ("src/source/pine_state_hash.cpp",
             "void source::PineStrategyHost::hash_host_extension(",
             "void source::PineStrategyHost::hash_source_extension("),
            ("include/pineforge/source/pine_strategy_host.hpp",
             "void hash_source_extension(BrokerStateHashSink& sink) const final {",
             "void hash_source_extension(BrokerStateHashSink& sink) const override {"),
        ):
            with self.subTest(relative=relative, before=before):
                root = self.copied_root()
                path = root / relative
                text = path.read_text()
                self.assertIn(before, text)
                path.write_text(text.replace(before, after, 1))
                with self.assertRaises(ValueError):
                    checker.check(root)

    def test_real_link_accept_and_reject_pair(self) -> None:
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        self.assertEqual(run_synthetic_pair(compiler), ["accept", "reject"])


if __name__ == "__main__":
    unittest.main()
