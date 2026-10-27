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
        # each 1024-byte object chunk is one record that rounds its own value
        # blob to 56-byte cells; cells (key + value) are packed 63 to a 4096-byte
        # page (ceil(cells * 4096 / 63)); each record adds a 50-byte index entry.
        rows = [
            (0, 1, 0, 181, 0),
            (1, 0, 0, 181, 0),
            (1, 1, 0, 181, 181),      # 2 cells -> 131, + 50
            (1, 1, 40, 181, 181),
            (1, 1, 41, 246, 246),     # 3 cells -> 196, + 50
            (1, 1, 96, 246, 246),
            (1, 1, 97, 311, 311),     # 4 cells -> 261, + 50
            (1, 1, 1000, 1351, 1351),  # 20 cells -> 1301, + 50
            (1, 1, 5000, 6622, 6622),  # 98 cells -> 6372, + 5 * 50
            (1, 2, 0, 181, 362),
            (1, 3, 0, 181, 543),
            (200000, 1, 0, 181, 36200000),
            (1000000, 1, 0, 181, 181000000),
            (1, 1, 0, 181, 181),
            (0, 1, 5000, 6622, 0),
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
            "kObjectPageBytes": "OBJECT_PAGE_BYTES",
            "kObjectCellsPerPage": "OBJECT_CELLS_PER_PAGE",
            "kObjectCellPayloadBytes": "OBJECT_CELL_PAYLOAD_BYTES",
            "kObjectChunkBytes": "OBJECT_CHUNK_BYTES",
            "kObjectInlinePayloadBytes": "OBJECT_INLINE_PAYLOAD_BYTES",
            "kObjectIndexShareBytes": "OBJECT_INDEX_SHARE_BYTES",
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
