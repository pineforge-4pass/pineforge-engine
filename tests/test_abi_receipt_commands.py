"""The actual receipt programs decide which options a registration needs."""
import argparse
import contextlib
import io
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import check_abi_receipt_skips as checker
import test_abi_receipt_registry as registry_tests
import test_abi_receipt_skips as receipt_tests
from prepare_settlement_cpp_abi_base import PROVIDERS, read_cache

CTEST = 'ctest'
BUILD = None
SCRIPTS = checker.ROOT / 'scripts'


class ReceiptCommands(unittest.TestCase):
    setUp = registry_tests.ReceiptRegistry.setUp
    inspect = registry_tests.ReceiptRegistry.inspect

    def budget(self):
        database = self.build / 'compile_commands.json'
        database.write_text('[]')
        return [sys.executable, str(SCRIPTS / 'check_runtime_budget.py'),
                '--candidate', sys.executable, '--source', __file__, '--csv', str(self.receipt),
                '--compiler', 'c++', '--generated-include', str(self.build),
                '--v16-frozen-receipt', str(self.receipt), '--require-receipts',
                '--compile-commands', str(database)]

    def check_command(self, command, name, *, reason=None):
        code, out = self.inspect({
            'name': name, 'command': command,
            'properties': [{'name': 'WORKING_DIRECTORY', 'value': str(self.build)}]})
        if reason is None:
            self.assertEqual(code, 0, out)
            self.assertRegex(out, rf'inputs present +{name}\b')
            self.assertIn('execution: NOT COMPARED', out)
        else:
            self.assertEqual(code, 2, out)
            self.assertIn(f'NOT COMPARED: {name}:', out)
            self.assertIn(reason, out)
            self.assertNotIn('inputs present', out)

    def test_budget_requires_the_compile_database_option(self):
        command = self.budget()
        self.check_command(command, 'budget')
        self.check_command(command[:-2] + ['--compile-commands=' + command[-1]], 'budget')
        self.check_command(command[:-2], 'budget', reason='--compile-commands')

    def test_settlement_requires_each_provider_option(self):
        common = [sys.executable, str(SCRIPTS / 'check_settlement_cpp_abi.py'),
                  '--compiler', 'c++', '--library', sys.executable, '--include', str(self.build),
                  '--generated-include', str(self.build), '--receipt', str(self.build / 'out.json'),
                  '--skip-if-receipt-missing', '--base-receipt', str(self.receipt)]
        rest = []
        for stem in ('prior', 'v13', 'v14', 'v15-frozen', 'v16-frozen', 'v18-frozen'):
            path = self.build / (stem + '.json')
            path.write_text('{}')
            rest.extend(['--' + stem + '-receipt', str(path)])
        self.check_command(common + rest, 'settlement')
        with self.subTest(only='--base-receipt'):
            self.check_command(common, 'settlement', reason='--prior-receipt')
        inputs = common[-2:] + rest
        for pos in range(0, len(inputs), 2):
            with self.subTest(omitted=inputs[pos]):
                self.check_command(common[:-2] + inputs[:pos] + inputs[pos + 2:], 'settlement',
                                   reason=inputs[pos])

    def test_relative_executable_must_exist_in_the_ctest_working_directory(self):
        directory = self.build / 'tools'
        directory.mkdir()
        (directory / 'python').symlink_to(sys.executable)
        command = self.budget()
        command[0] = './tools/python'
        self.check_command(command, 'relative')
        (directory / 'python').unlink()
        with self.subTest(path='absent'):
            self.check_command(command, 'relative', reason='./tools/python')
        (directory / 'python').write_text('not an executable\n')
        (directory / 'python').chmod(0o644)
        with self.subTest(path='not executable'):
            self.check_command(command, 'relative', reason='./tools/python')

    def test_aggregate_runtime_rejects_an_incomplete_option_group(self):
        command = receipt_tests.receipt_command(self.build, 'check_aggregate_cpp_versions.py',
                                                ['--v18-frozen-receipt', str(self.receipt)])
        self.check_command(command, 'aggregate')
        library = command.index('--library')
        self.check_command(command[:library] + command[library + 2:], 'aggregate', reason='--library')

    def test_a_different_script_with_the_same_name_cannot_supply_the_parser(self):
        command = self.budget()
        script = self.build / Path(command[1]).name
        script.write_text('raise RuntimeError("registry programs must not execute")\n')
        command[1] = str(script)
        self.check_command(command, 'foreign', reason=str(script))


class RepositoryCommands(unittest.TestCase):
    def test_receipt_inputs_in_the_configured_registry(self):
        self.assertIsNotNone(BUILD, 'the repository control requires --build-dir')
        cache = read_cache(BUILD / 'CMakeCache.txt')
        self.assertEqual(Path(cache['CMAKE_HOME_DIRECTORY']).resolve(), checker.ROOT)
        tests = checker.registered_tests(BUILD, CTEST)
        registry = {test['name']: test for test in tests}
        self.assertIn('test_abi_receipt_skips', registry)
        layer = cache['PINEFORGE_BUILD_SOURCE_LAYER']
        self.assertIn(layer, ('ON', 'OFF'))
        want = {}
        output = io.StringIO()
        for name, roles in receipt_tests.PrivateFixtureManifests.ROW_PROVIDERS.items():
            if layer == 'OFF':
                self.assertNotIn(name, registry)
                continue
            command = registry[name]['command']
            if not all(PROVIDERS[role]['manifest'].is_file() for role in roles):
                self.assertIn('SKIP private fixtures not present: ', ' '.join(command))
                continue
            paths = []
            for role in roles:
                stem = {'e60': 'base', '0e': 'prior'}.get(role, role)
                flag = '--' + stem + '-receipt'
                self.assertIn(flag, command, name)
                paths.append(Path(command[command.index(flag) + 1]))
            database = None
            if name == 'test_l4g_runtime_budget':
                database = Path(command[command.index('--compile-commands') + 1])
            mode = 'require' if cache['PINEFORGE_REQUIRE_ABI_RECEIPTS'] == 'ON' else 'skip'
            want[name] = (mode, [p for p in paths if not p.is_file()],
                          database if database and not database.is_file() else None)
        rows = checker.gated_rows(tests)
        self.assertEqual({row.name: (row.mode, list(row.missing), row.database) for row in rows}, want)
        with contextlib.redirect_stdout(output):
            code = checker.report(rows, BUILD, str(BUILD))
        self.assertEqual(code, int(any(paths or db for _, paths, db in want.values())))
        self.assertIn('execution: NOT COMPARED', output.getvalue())
        for name, (_, paths, db) in want.items():
            verdict = 'inputs missing' if paths or db else 'inputs present'
            self.assertRegex(output.getvalue(), rf'{verdict} +{name}\b')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ctest', required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    options, rest = parser.parse_known_args()
    CTEST, BUILD = options.ctest, options.build_dir
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
