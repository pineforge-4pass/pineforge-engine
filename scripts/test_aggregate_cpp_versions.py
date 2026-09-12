#!/usr/bin/env python3
"""Mutation controls for source ABI ownership; no compiler or engine runs."""
import unittest
from check_aggregate_cpp_versions import check_texts, load

DATA = load()


class AggregateVersions(unittest.TestCase):
    def reject(self, path, before, after):
        self.assertIn(before, DATA[path])
        changed = dict(DATA)
        changed[path] = changed[path].replace(before, after)
        with self.assertRaises(ValueError):
            check_texts(changed)

    def test_current(self):
        check_texts(DATA)

    def test_engine_and_forward_declarations(self):
        for path, text in DATA.items():
            if 'engine_script_run_v12' in text:
                with self.subTest(path=path):
                    self.reject(path, 'engine_script_run_v12', 'engine_script_run_v11')

    def test_broker_and_stream_entry_points(self):
        self.reject('src/engine_state_hash.cpp', 'pineforge-broker-state/v12', 'pineforge-broker-state/v11')
        self.reject('src/engine_stream.cpp', 'integer(12); integer(broker_state_hash());',
                    'integer(11); integer(broker_state_hash());')
        self.reject('src/engine_stream.cpp', 'integer(12); integer(broker_state_hash());',
                    'if (false) { integer(12); integer(broker_state_hash()); }')

    def test_standalone_owners(self):
        for path, namespace in (
            ('include/pineforge/exit_leg_lifecycle.hpp', 'lifecycle_v1'),
            ('include/pineforge/market_admission.hpp', 'market_admission_v2'),
            ('src/market_admission.cpp', 'market_admission_v2'),
            ('include/pineforge/reservation_expansion.hpp', 'reservation_expansion_v1'),
            ('src/reservation_expansion.cpp', 'reservation_expansion_v1'),
        ):
            with self.subTest(path=path):
                replacement = (namespace.replace('_v2', '_v3')
                               if namespace.endswith('_v2')
                               else namespace.replace('_v1', '_v2'))
                self.reject(path, namespace, replacement)

    def test_empty_namespace_is_not_ownership(self):
        for path, namespace in (
            ('include/pineforge/exit_leg_lifecycle.hpp', 'lifecycle_v1'),
            ('include/pineforge/market_admission.hpp', 'market_admission_v2'),
        ):
            self.reject(path, 'inline namespace ' + namespace + ' {',
                        'inline namespace ' + namespace + ' {} namespace misplaced {')


if __name__ == '__main__':
    unittest.main()
