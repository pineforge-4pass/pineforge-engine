#!/usr/bin/env python3
"""Prove the CTest probe rows cover the equivalence driver's cases once."""

import ast
from collections import Counter
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).absolute().parents[1]
PROBE_LIST = re.compile(r'\bset\(\s*_pf_equivalence_probes\s+([^)]*)\)', re.S)
PROBE_LOOP = re.compile(
    r'\bforeach\(\s*_pf_equivalence_probe\s+IN\s+LISTS\s+_pf_equivalence_probes\s*\)'
    r'(.*?)\bendforeach\(\s*\)', re.S)
ROW_NAME = 'native_live_equivalence_e2e_${_pf_equivalence_probe}'


def driver_probes(source):
    declarations = [node for node in ast.parse(source).body
                    if isinstance(node, ast.Assign) and any(
                        isinstance(target, ast.Name) and target.id == 'PROBES'
                        for target in node.targets)]
    if len(declarations) != 1 or not isinstance(declarations[0].value, ast.Dict):
        raise ValueError('expected one literal PROBES dictionary')
    probes = [ast.literal_eval(key) for key in declarations[0].value.keys]
    if (not probes or any(not isinstance(probe, str) or not probe for probe in probes)
            or len(probes) != len(set(probes))):
        raise ValueError('PROBES must contain distinct, nonempty string keys')
    return probes


def check_cover(probes, cmake):
    # Read only the literal registration list, never execute CMake or import
    # the driver. Check the loop wiring too: a matching unused list proves no cases.
    cmake = re.sub(r'#[^\n]*', '', cmake)
    lists = list(PROBE_LIST.finditer(cmake))
    rows = lists[0].group(1).split() if len(lists) == 1 else []
    counts = Counter(rows)
    expected = set(probes)
    missing = sorted(expected - counts.keys())
    duplicates = sorted(probe for probe, count in counts.items() if count != 1)
    unknown = sorted(counts.keys() - expected)
    errors = []
    for description, names in (
            ('missing probe rows', missing), ('duplicate probe rows', duplicates),
            ('unknown probe rows', unknown)):
        if names:
            errors.append(description + ': ' + ', '.join(names))
    if len(lists) != 1:
        errors.append('expected one CMake probe list')
    loops = PROBE_LOOP.findall(cmake)
    declarations = re.findall(
        r'\badd_test\(\s*NAME\s+(native_live_equivalence_e2e[^\s)]*)\s+(.*?)\)',
        cmake, re.S)
    if len(loops) != 1 or len(declarations) != 1 or declarations[0][0] != ROW_NAME:
        errors.append('expected only the per-probe equivalence registration loop')
    else:
        name, command = declarations[0]
        loop_rows = re.findall(r'\badd_test\(\s*NAME\s+([^\s)]+)', loops[0])
        if loop_rows != [name]:
            errors.append('the probe list must drive the equivalence row')
        selections = re.findall(
            r'--probes\b(.*?)(?=\s+--|\s+\$\{_pf_genuine_retry_sanitizer_args\}|$)',
            command, re.S)
        if [selection.split() for selection in selections] != [['${_pf_equivalence_probe}']]:
            errors.append('each row must forward exactly its selected probe')
        if re.search(r'--(?:timeframes|qualification-only|ticks(?:-only)?|reuse-libraries)\b',
                     command):
            errors.append('each probe must keep the default timeframe and scenario population')
        if '${PROJECT_SOURCE_DIR}/tests/native_live_equivalence_ctest.py' not in command:
            errors.append('each probe must use the sanitizer and prerequisite adapter')
    if errors:
        raise ValueError('; '.join(errors))


def read_inputs():
    paths = [ROOT / 'tests' / 'native_live_equivalence_e2e.py',
             ROOT / 'runner' / 'CMakeLists.txt']
    print('Equivalence cover inputs: ' + ', '.join(map(str, paths)), flush=True)
    for path in paths:
        # Refuse symlinks from the root down instead of following an input
        # into a protected directory before checking its resolved name.
        for candidate in reversed((path, *path.parents)):
            parts = candidate.parts
            if (any(part in ('.ssh', '.aws', '.codex') for part in parts)
                    or candidate.name.endswith('.env')
                    or '/.config/zsh/keys/' in candidate.as_posix() + '/'
                    or '/.config/pineforge/' in candidate.as_posix() + '/'):
                raise ValueError(f'refusing protected input path: {candidate}')
            if candidate.is_symlink():
                raise ValueError(f'refusing symbolic input path: {candidate}')
    return [path.read_text() for path in paths]


class NativeLiveEquivalenceSplit(unittest.TestCase):
    def test_probe_rows_cover_every_case_once_and_reject_mutations(self):
        driver, cmake = read_inputs()
        probes = driver_probes(driver)
        check_cover(probes, cmake)
        population = PROBE_LIST.search(cmake)

        def replace_population(rows):
            return cmake[:population.start(1)] + '\n'.join(rows) + cmake[population.end(1):]

        for probe in probes:
            with self.subTest(mutation='drop', probe=probe):
                with self.assertRaisesRegex(ValueError, 'missing probe rows: ' + re.escape(probe)):
                    check_cover(probes, replace_population([row for row in probes if row != probe]))
            with self.subTest(mutation='duplicate', probe=probe):
                with self.assertRaisesRegex(ValueError, 'duplicate probe rows: ' + re.escape(probe)):
                    check_cover(probes, replace_population([*probes, probe]))
        unknown = probes[0] + '-unknown'
        with self.assertRaisesRegex(ValueError, 'unknown probe rows: ' + re.escape(unknown)):
            check_cover(probes, replace_population([*probes, unknown]))
        with self.assertRaisesRegex(ValueError, 'missing probe rows:'):
            check_cover(probes, replace_population([]))
        with self.assertRaisesRegex(ValueError, 'only the per-probe equivalence registration'):
            check_cover(probes, cmake + '\nadd_test(NAME native_live_equivalence_e2e COMMAND old)\n')
        with self.assertRaisesRegex(ValueError, 'forward exactly its selected probe'):
            check_cover(probes, cmake.replace('--probes ${_pf_equivalence_probe}', '', 1))
        with self.assertRaisesRegex(ValueError, 'forward exactly its selected probe'):
            check_cover(probes, cmake.replace('--probes ${_pf_equivalence_probe}',
                                              '--probes ${_pf_equivalence_probe} ' + probes[0], 1))


if __name__ == '__main__':
    unittest.main()
