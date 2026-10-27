#!/usr/bin/env python3
"""Hand-derived cross-language rows for object-store arena reservation.

This is intentionally red on the pre-reservation head because the Python
mirror functions are absent there.
"""
from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "pineforge" / "detail" / "scratch_defaults.hpp"
RUN_JSON = ROOT / "docker" / "run_json.py"


def load_run_json():
    spec = importlib.util.spec_from_file_location("pineforge_run_json", RUN_JSON)
    if spec is None or spec.loader is None:
        raise ImportError("cannot load docker/run_json.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ObjectStoreReserveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.run_json = load_run_json()

    def test_hand_derived_rows_match_the_python_mirror(self):
        # The expected values are written independently from the C++ rows:
        # each later 1024-byte object chunk rounds its own value blob.
        rows = [
            (0, 1, 0, 168, 0),
            (1, 0, 0, 168, 0),
            (1, 1, 0, 168, 168),
            (1, 1, 40, 168, 168),
            (1, 1, 41, 232, 232),
            (1, 1, 96, 232, 232),
            (1, 1, 97, 296, 296),
            (1, 1, 1000, 1320, 1320),
            (1, 1, 5000, 6312, 6312),
            (1, 2, 0, 168, 336),
            (1, 3, 0, 168, 504),
            (200000, 1, 0, 168, 33600000),
            (1000000, 1, 0, 168, 168000000),
            (1, 1, 0, 168, 168),
            (0, 1, 5000, 6312, 0),
        ]
        for bars, executions, payload, physical, reserve in rows:
            self.assertEqual(
                self.run_json.object_physical_bytes(payload), physical)
            self.assertEqual(
                self.run_json.object_store_reserve_bytes(
                    bars, executions, payload),
                reserve)

    def test_saturation_and_unbounded_sentinel(self):
        maximum = 2 ** 64 - 1
        self.assertEqual(
            self.run_json.object_store_reserve_bytes(maximum // 2, 4, 0),
            maximum)
        self.assertEqual(
            self.run_json.object_store_reserve_bytes(1, maximum, 0),
            maximum)
        self.assertEqual(
            self.run_json.object_store_reserve_bytes(0, maximum, 0),
            0)

    def test_header_and_python_constants_do_not_drift(self):
        header_text = HEADER.read_text(encoding="utf-8")
        pairs = {
            "kObjectHeaderBytes": "OBJECT_HEADER_BYTES",
            "kObjectKeyBytes": "OBJECT_KEY_BYTES",
            "kObjectCellBytes": "OBJECT_CELL_BYTES",
            "kObjectCellPayloadBytes": "OBJECT_CELL_PAYLOAD_BYTES",
            "kObjectChunkBytes": "OBJECT_CHUNK_BYTES",
            "kObjectInlinePayloadBytes": "OBJECT_INLINE_PAYLOAD_BYTES",
            "kObjectIndexShareBytes": "OBJECT_INDEX_SHARE_BYTES",
            "kObjectSmallBytes": "OBJECT_SMALL_BYTES",
            "kObjectArenaChunkBytes": "OBJECT_ARENA_CHUNK_BYTES",
            "kObjectArenaDirectoryBytesPerChunk":
                "OBJECT_ARENA_DIRECTORY_BYTES_PER_CHUNK",
        }
        for cpp_name, python_name in pairs.items():
            match = re.search(
                rf"inline constexpr std::uint64_t {cpp_name}\s*=\s*(\d+);",
                header_text)
            self.assertIsNotNone(match, cpp_name)
            self.assertEqual(getattr(self.run_json, python_name), int(match.group(1)))


if __name__ == "__main__":
    unittest.main()
