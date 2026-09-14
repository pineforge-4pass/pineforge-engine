#!/usr/bin/env python3
"""Metadata-only mutations of the exact nested reservation mirror contract."""
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import gen_pending_order_mirror as mirror

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / 'include/pineforge/reservation_expansion.hpp').read_text()

class ReservationMirror(unittest.TestCase):
    def test_complete_schema_and_generated_output(self):
        header, descriptor, projection = mirror.generate_parts()
        self.assertEqual(header, mirror.OUT_H.read_text())
        self.assertEqual(descriptor, mirror.OUT_C.read_text())
        self.assertEqual(projection, mirror.OUT_SOURCE_C.read_text())
        self.assertIn('#define PF_PENDING_ORDER_FIELD_COUNT 406', header)
        self.assertIn('406 POD fields', header)

    def test_every_nested_mapping_is_observable(self):
        original = mirror.generate_parts()
        for typename in ['ReservationExpansion', 'ReservationGrowthSource']:
            values = mirror.COMPOSITE_MAP[typename]
            for index, (name, kind, expression) in enumerate(values):
                for mutation in ['omit', 'name', 'type', 'value']:
                    with self.subTest(typename=typename, field=name, mutation=mutation):
                        changed = values.copy()
                        if mutation == 'omit': del changed[index]
                        if mutation == 'name': changed[index] = ('wrong_' + name, kind, expression)
                        if mutation == 'type': changed[index] = (name, 'double' if kind != 'double' else 'int64_t', expression)
                        if mutation == 'value': changed[index] = (name, kind, '0')
                        with patch.dict(mirror.COMPOSITE_MAP, {typename:changed}):
                            self.assertNotEqual(mirror.generate_parts(), original)
                            with redirect_stdout(StringIO()): self.assertEqual(mirror.main(['--check']), 1)

    def test_legacy_projection_mutations_are_observable(self):
        for field in ['pooc_global_full_exit_dynamic_qty', 'pooc_global_full_exit_tracks_bound_adds', 'pooc_global_full_exit_bound_add']:
            with self.subTest(field=field), patch.dict(mirror.LEGACY_OUTPUTS, {field:'0'}), redirect_stdout(StringIO()):
                self.assertEqual(mirror.main(['--check']),1)

    def test_nested_storage_growth_fails_generation(self):
        for declaration in ['std::optional<ReservationExpansionCapture> capture_;',
                            'int64_t position_cycle;', 'PositionSide side;',
                            'std::optional<uint64_t> first_later_admission;',
                            'std::optional<uint64_t> reservation_owner_;']:
            with self.subTest(declaration=declaration), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                path = root / 'include/pineforge/reservation_expansion.hpp'
                path.parent.mkdir(parents=True)
                (path.parent / "exit_leg_lifecycle.hpp").write_text((ROOT / "include/pineforge/exit_leg_lifecycle.hpp").read_text())
                path.write_text(HEADER.replace(declaration,declaration + ' int hidden;'))
                with patch.object(mirror,'ROOT',root), self.assertRaises(ValueError): mirror.generate()

    def test_receipts_cannot_be_waived(self):
        for field in ['reservation_expansion','reservation_growth_source']:
            with self.subTest(field=field), self.assertRaises(SystemExit), redirect_stderr(StringIO()):
                mirror.classify(mirror.members(),{field:'forbidden'})

if __name__ == '__main__': unittest.main()
