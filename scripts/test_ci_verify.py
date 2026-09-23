#!/usr/bin/env python3
"""Driver control-flow, config, aggregation, and receipt-reuse tests for ci_verify.

Source guards, tool help, compiler identity, and VERSION are real. The scripted
driver has an explicit Git object inventory; a separate tiny Git fixture checks
real object discovery. Full engine configure/build/CTest is not substituted as a passing
root verification — those stages are scripted here so failure aggregation and
ordering can be asserted. Root must still run the actual profiles.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import check_abi_receipt_skips
import ci_verify
from ci_verify import (
    Completed,
    ConfigError,
    KERNEL_MIN_TESTS,
    PROFILES,
    RELEASE_MIN_TESTS,
    ROOT,
    SANITIZER_FLAG,
    SANITIZER_RUN_ENV,
    cmake_cache_definitions,
    cmake_configure_argv,
    ctest_row_count,
    ctest_rows,
    ctest_supports_junit,
    default_build_dir,
    default_runner,
    expected_version,
    main,
    native_include_independence_command,
    parse_args,
    source_guard_commands,
    validate_config,
)
from prepare_settlement_cpp_abi_base import (
    BASE_COMMIT,
    BASE_TREE,
    PRIOR_COMMIT,
    PRIOR_TREE,
    V13_COMMIT,
    V13_TREE,
    V14_COMMIT,
    V14_TREE,
    V15_FROZEN_COMMIT,
    V15_FROZEN_TREE,
    V16_FROZEN_COMMIT,
    V16_FROZEN_TREE,
    V18_FROZEN_COMMIT,
    V18_FROZEN_TREE,
    PROVIDERS,
    COPY_CACHE,
    authenticate_headers,
    compiler_identity,
    copied_cache,
    extract_tar,
    identity,
    receipt_matches_current,
    reusable_prepared_base,
)


def write_cache(path: Path, values: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [f'{key}:STRING={value}' for key, value in values.items()]
    path.write_text('\n'.join(lines) + '\n')


def which_with_ccache(ccache: str | None):
    def which(name: str) -> str | None:
        if name == 'ccache':
            return ccache
        return shutil.which(name)
    return which


def ctest_output(total: int, skipped=(), not_run=()) -> bytes:
    """CTest 4.4's console shape for a run of `total` rows, as ci_verify reads it.

    Each skipped or not-run row prints its own result line during the run and
    is listed again after the closing summary, which counts it inside `total`
    (a skipped row as passed, a not-run row as failed).
    """
    lines = ['Test project /scripted/build']
    for number, name in enumerate(skipped, 1):
        lines.append(f'{number:3}/{total} Test #{number}: {name} ........***Skipped   0.01 sec')
    for number, name in enumerate(not_run, len(skipped) + 1):
        lines.append(f'Unable to find executable: {name}')
        lines.append(f'{number:3}/{total} Test #{number}: {name} ........***Not Run   0.00 sec')
    if not_run:
        percent = min(99, (total - len(not_run)) * 100 // total)
        lines.append(f'\n{percent}% tests passed, {len(not_run)} tests failed out of {total}')
    else:
        lines.append(f'\n100% tests passed out of {total}')
    lines.append('\nTotal Test time (real) =   1.00 sec')
    if skipped:
        lines.append('\nThe following tests did not run:')
        lines += [f'\t{number:3} - {name} (Skipped)' for number, name in enumerate(skipped, 1)]
    if not_run:
        lines.append('\nThe following tests FAILED:')
        lines += [f'\t{number:3} - {name} (Not Run)'
                  for number, name in enumerate(not_run, len(skipped) + 1)]
        lines.append('Errors while running CTest')
    return ('\n'.join(lines) + '\n').encode()


class Scripted:
    """Programmable command runner that still executes real source guards/tools."""

    def __init__(self, build_dir: Path, source: Path, profile: str = 'release', **exits):
        self.build_dir = Path(build_dir)
        self.source = Path(source)
        self.profile = profile
        self.exits = exits
        self.calls: list[list[str]] = []
        self.cxx = shutil.which('c++') or '/usr/bin/c++'
        self.secret_seen_in_argv = False

    def __call__(self, argv, *, extra_env=None, timeout=600, combine_stderr=True,
                 stream_output=False) -> Completed:
        argv = list(map(str, argv))
        self.calls.append(argv)
        extra_env = extra_env or {}
        joined = ' '.join(argv)
        secret = os.environ.get('CI_VERIFY_TEST_SECRET')
        if secret and secret in joined:
            self.secret_seen_in_argv = True
        if argv[0] == 'ctest' and '--help' in argv:
            if self.exits.get('junit_help') == 'absent':
                return Completed(0, b'CTest help without junit\n', b'')
            return default_runner(argv, extra_env=None, timeout=timeout,
                                  combine_stderr=True, stream_output=False)
        if argv[0] in {'cmake', 'ctest', 'git'} and '--version' in argv:
            return default_runner(argv, extra_env=None, timeout=timeout,
                                  combine_stderr=True, stream_output=False)
        if argv[0] == 'git' and 'submodule' in argv:
            if 'update' in argv:
                return Completed(int(self.exits.get('corpus-submodule-init', 0)),
                                 b'corpus initialized\n', b'')
            if 'status' in argv:
                prefix = str(self.exits.get('corpus-submodule-prefix', ' '))
                return Completed(0, (prefix + 'b46cd80c247a53b19e23cb0c12c4451d624ce9a6 corpus\n').encode(), b'')
        if argv[0] == 'git' and 'cat-file' in argv:
            if V18_FROZEN_COMMIT + '^{commit}' in argv:
                key = 'v18-frozen-cat-file'
            elif V16_FROZEN_COMMIT + '^{commit}' in argv:
                key = 'v16-frozen-cat-file'
            elif V15_FROZEN_COMMIT + '^{commit}' in argv:
                key = 'v15-frozen-cat-file'
            elif V14_COMMIT + '^{commit}' in argv:
                key = 'v14-cat-file'
            elif V13_COMMIT + '^{commit}' in argv:
                key = 'v13-cat-file'
            elif PRIOR_COMMIT + '^{commit}' in argv:
                key = 'prior-cat-file'
            elif BASE_COMMIT + '^{commit}' in argv:
                key = 'cat-file'
            else:
                return Completed(128, b'', b'unknown fixture object\n')
            # Control-flow tests start with all providers present; individual
            # tests explicitly remove one. Never depend on checkout depth or
            # on a previous full verifier run having fetched old commits.
            return Completed(int(self.exits.get(key, 0)), b'', b'')
        if argv[0] == 'git' and 'fetch' in argv:
            key = self.provider_command_name(argv, 'fetch')
            return Completed(int(self.exits.get(key, 0)), b'fetched\n', b'')
        if any(Path(part).name == 'prepare_settlement_cpp_abi_base.py' for part in argv):
            key = self.provider_command_name(argv, 'prepare')
            return Completed(int(self.exits.get(key, 0)), b'prepared\n', b'')
        if any(Path(part).name == 'check_native_include_independence.py' for part in argv):
            return Completed(int(self.exits.get('native-include-independence', 0)),
                             b'native include independence\n', b'')
        if any(Path(part).name == 'check_kernel_residuals.py' for part in argv):
            return Completed(int(self.exits.get('kernel-residuals', 0)),
                             b'kernel residuals\n', b'')
        if any(Path(part).suffix == '.py' for part in argv):
            needles = {
                'source-guard-c-abi': 'check_c_abi_runtime.py',
                'source-guard-native-source': 'test_native_source_guard.py',
                'source-guard-broker-hash': 'check_broker_state_hash_coverage.py',
                'source-guard-pending-mirror': 'gen_pending_order_mirror.py',
                'source-guard-native-versions': 'check_native_cpp_versions.py',
                'source-guard-aggregate-versions': 'check_aggregate_cpp_versions.py',
                'source-guard-adapter-spec-shadowing': 'check_adapter_spec_shadowing.py',
                'source-guard-twin-parity': 'check_twin_parity.py',
            }
            for key, needle in needles.items():
                if key in self.exits and any(needle in part for part in argv):
                    return Completed(int(self.exits[key]), b'', b'injected source-guard failure\n')
            return default_runner(argv, extra_env=None, timeout=timeout,
                                  combine_stderr=True, stream_output=False)
        if argv[0] == 'cmake' and '-S' in argv and 'smoke_consumer' in joined:
            return Completed(int(self.exits.get('smoke-configure', 0)), b'ok\n', b'')
        if argv[0] == 'cmake' and '-S' in argv:
            return self._configure()
        if argv[0] == 'cmake' and '--build' in argv and str(self.build_dir / 'ci-smoke') in argv:
            return Completed(int(self.exits.get('smoke-build', 0)), b'ok\n', b'')
        if argv[0] == 'cmake' and '--build' in argv:
            return self._build()
        if argv[0] == 'cmake' and '--install' in argv:
            return self._install()
        if argv[0] == 'ctest':
            if self.exits.get('actual_empty_ctest'):
                return default_runner(argv, extra_env=extra_env, timeout=timeout,
                                      combine_stderr=combine_stderr, stream_output=False)
            env_ok = True
            if self.profile == 'sanitizers':
                env_ok = extra_env == SANITIZER_RUN_ENV
            if not env_ok:
                return Completed(1, b'', b'sanitizer env missing\n')
            # CTest's closing summary carries the row count the floor reads;
            # 'absent' scripts a run that never printed one. By default a
            # profile reports exactly its own floor. 'ctest_skipped' and
            # 'ctest_not_run' name rows of that count that did not run;
            # 'ctest_raw' scripts the output verbatim.
            if 'ctest_raw' in self.exits:
                return Completed(int(self.exits.get('ctest', 0)), self.exits['ctest_raw'], b'')
            rows = self.exits.get(
                'ctest_rows', ci_verify.PROFILE[self.profile].min_tests or KERNEL_MIN_TESTS)
            if rows == 'absent':
                return Completed(int(self.exits.get('ctest', 0)), b'tests\n', b'')
            summary = ctest_output(rows, self.exits.get('ctest_skipped', ()),
                                   self.exits.get('ctest_not_run', ()))
            return Completed(int(self.exits.get('ctest', 0)), summary, b'')
        if argv[0].endswith('smoke_version') or Path(argv[0]).name == 'smoke_version':
            stdout = str(self.exits.get('smoke-stdout', expected_version(self.source))).encode() + b'\n'
            stderr = str(self.exits.get('smoke-stderr', '')).encode()
            return Completed(int(self.exits.get('smoke-version', 0)), stdout, stderr)
        if Path(argv[0]).name == 'pineforge-live':
            return Completed(int(self.exits.get('native-help', 0)), b'usage\n', b'')
        if Path(argv[0]).name == 'test_native_live_websocket':
            return Completed(int(self.exits.get('require-websocket', 0)), b'ws\n', b'')
        if '--version' in argv:
            return Completed(0, b'version\n', b'')
        return Completed(1, b'', f'unhandled command: {argv}\n'.encode())

    def _cache_values(self) -> dict[str, str]:
        tutorial = 'OFF' if self.profile in {'native', 'kernel'} else 'ON'
        live = 'ON' if self.profile in {'native', 'kernel'} else 'OFF'
        sanitizers = 'ON' if self.profile == 'sanitizers' else 'OFF'
        source_layer = 'OFF' if self.profile == 'kernel' else 'ON'
        examples = 'ON' if self.profile in {'release', 'kernel'} else 'OFF'
        build_type = 'Debug' if self.profile in {'debug', 'sanitizers'} else 'Release'
        values = {
            'CMAKE_HOME_DIRECTORY': str(self.source),
            'CMAKE_CXX_COMPILER': self.cxx,
            'CMAKE_C_COMPILER': self.cxx,
            'CMAKE_GENERATOR': 'Unix Makefiles',
            'CMAKE_BUILD_TYPE': build_type,
            'PINEFORGE_BUILD_TESTS': 'ON',
            'PINEFORGE_BUILD_TUTORIAL': tutorial,
            'PINEFORGE_BUILD_LIVE_RUNNER': live,
            'PINEFORGE_BUILD_SOURCE_LAYER': source_layer,
            'PINEFORGE_ENABLE_SANITIZERS': sanitizers,
            'PINEFORGE_BUILD_EXAMPLES': examples,
            'PINEFORGE_REQUIRE_ABI_RECEIPTS': 'ON',
            'PINEFORGE_VERSION_SOURCE': 'FILE',
            'Python3_EXECUTABLE': self.exits.get('cache_python', sys.executable),
        }
        if self.exits.get('cache_version_source'):
            values['PINEFORGE_VERSION_SOURCE'] = self.exits['cache_version_source']
        if self.exits.get('launcher'):
            values['CMAKE_C_COMPILER_LAUNCHER'] = self.exits['launcher']
            values['CMAKE_CXX_COMPILER_LAUNCHER'] = self.exits['launcher']
        return values

    def _configure(self) -> Completed:
        code = int(self.exits.get('configure', 0))
        if code != 0:
            return Completed(code, b'', b'configure failed\n')
        write_cache(self.build_dir / 'CMakeCache.txt', self._cache_values())
        if self.exits.get('actual_empty_ctest'):
            # A real, empty CTest inventory proves the no-test exit policy.
            # Configure/build remain scripted; no engine executable is run.
            (self.build_dir / 'CTestTestfile.cmake').write_text('# deliberately empty inventory\n')
        # 'example_commands' == 'absent' scripts a configure that wrote no
        # compile database at all.
        if self.exits.get('example_commands') != 'absent':
            commands = self._library_compile_commands() + self._example_compile_commands()
            (self.build_dir / 'compile_commands.json').write_text(json.dumps(commands))
        for role in ('e60', '0e', 'v13', 'v14', 'v15-frozen', 'v16-frozen', 'v18-frozen'):
            self._maybe_seed_abi_base(role)
        return Completed(0, b'configured\n', b'')

    def _library_compile_commands(self) -> list[dict]:
        """A real kernel translation unit, src/matrix.cpp, compiled into each of
        the two archives every profile builds, with the library's include
        directories; the sanitizers profile adds the PUBLIC sanitizer flag
        unless 'sanitizer_flag' is 'absent'. 'library_commands' == 'none'
        scripts a database without them."""
        if self.exits.get('library_commands') == 'none':
            return []
        flag = ''
        if self.profile == 'sanitizers' and self.exits.get('sanitizer_flag') != 'absent':
            flag = ' ' + SANITIZER_FLAG
        unit = self.source / 'src/matrix.cpp'
        commands = []
        for target in ('pineforge', 'pineforge_kernel'):
            obj = f'CMakeFiles/{target}.dir/src/matrix.cpp.o'
            commands.append({
                'directory': str(self.build_dir),
                'file': str(unit),
                'output': obj,
                'command': f'{self.cxx} -I{self.source}/include -I{self.build_dir}/include '
                           f'-I{self.source}/src -O2{flag} -o {obj} -c {unit}',
            })
        return commands

    def _example_compile_commands(self) -> list[dict]:
        """The compiles of examples/native sources, as CMake emits them: two
        example_* targets where the profile builds the examples, and the live
        runner's two MODULE builds of example sources where it builds the
        runner. Every one strips Release's NDEBUG with -UNDEBUG unless
        'example_ndebug' names its target; 'example_commands' == 'none'
        scripts a database without any."""
        if self.exits.get('example_commands') == 'none':
            return []
        examples = self.source / 'examples/native'
        rows = []
        if ci_verify.PROFILE[self.profile].live_runner:
            rows += [('runner', target, f'__/examples/native/{source}', source, ' -fPIC')
                     for target, source in (('native_market_example', 'native_market_strategy.cpp'),
                                            ('native_selected_example',
                                             'native_selected_strategy.cpp'))]
        if self.profile in ci_verify.EXAMPLES_PROFILES:
            rows += [('examples/native', target, source, source, '')
                     for target, source in (('example_hello_kernel', 'hello_kernel.cpp'),
                                            ('example_hello_kernel_c', 'hello_kernel_c.c'))]
        commands = []
        for directory, target, object_path, source, extra in rows:
            undebug = '' if self.exits.get('example_ndebug') == target else ' -UNDEBUG'
            obj = f'CMakeFiles/{target}.dir/{object_path}.o'
            commands.append({
                'directory': str(self.build_dir / directory),
                'file': str(examples / source),
                'output': f'{directory}/{obj}',
                'command': f'{self.cxx} -O3 -DNDEBUG -ffp-contract=off{extra}{undebug} -o '
                           f'{obj} -c {examples}/{source}',
            })
        return commands

    def _build(self) -> Completed:
        code = int(self.exits.get('build', 0))
        if code != 0:
            return Completed(code, b'', b'build failed\n')
        # Both archives every profile builds. 'stale_archive' names one the
        # scripted build leaves older than the sources it is compiled from.
        for name in ('libpineforge.a', 'libpineforge_kernel.a'):
            archive = self.build_dir / 'lib' / name
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(b'!<arch>\nci-verify-test\n')
            if self.exits.get('stale_archive') == name:
                os.utime(archive, (1, 1))
        if self.profile in {'native', 'kernel'} or self.exits.get('create_native_binaries'):
            binary = self.build_dir / 'bin' / 'pineforge-live'
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(b'live')
            ws = self.build_dir / 'bin' / 'test_native_live_websocket'
            ws.write_bytes(b'ws')
        if self.exits.get('omit_ws_binary'):
            ws = self.build_dir / 'bin' / 'test_native_live_websocket'
            if ws.exists():
                ws.unlink()
        return Completed(0, b'built\n', b'')

    def _install(self) -> Completed:
        code = int(self.exits.get('install', 0))
        if code != 0:
            return Completed(code, b'', b'install failed\n')
        prefix = self.build_dir / 'ci-install'
        (prefix / 'lib' / 'cmake' / 'PineForge').mkdir(parents=True, exist_ok=True)
        if self.profile in {'native', 'kernel'}:
            help_bin = prefix / 'bin' / 'pineforge-live'
            help_bin.parent.mkdir(parents=True, exist_ok=True)
            help_bin.write_bytes(b'live')
        (self.build_dir / 'ci-smoke').mkdir(parents=True, exist_ok=True)
        (self.build_dir / 'ci-smoke' / 'smoke_version').write_bytes(b'smoke')
        return Completed(0, b'installed\n', b'')

    def _maybe_seed_abi_base(self, role: str) -> None:
        key = {'e60': 'base', '0e': 'prior', 'v13': 'v13', 'v14': 'v14',
               'v15-frozen': 'v15_frozen', 'v16-frozen': 'v16_frozen',
               'v18-frozen': 'v18_frozen'}[role]
        kind = self.exits.get('preexisting_' + key)
        if not kind:
            return
        provider = PROVIDERS[role]
        output = self.build_dir / provider['default_output']
        output.mkdir(parents=True, exist_ok=True)
        (output / 'sentinel').write_text('keep\n')
        if kind == 'no-receipt':
            return
        archive = output / 'build' / 'lib' / 'libpineforge.a'
        headers = output / provider['headers_name']
        generated = output / 'build' / 'include' / 'pineforge' / 'version.h'
        archive.parent.mkdir(parents=True, exist_ok=True)
        generated.parent.mkdir(parents=True, exist_ok=True)
        archive.write_bytes(b'!<arch>\nbase\n')
        headers.write_bytes(b'headers')
        generated.write_text('#define PINEFORGE_VERSION_STRING "0.14.0"\n')
        compiler = compiler_identity(self.cxx)
        cache = copied_cache(self._cache_values())
        if kind == 'mismatch':
            cache = dict(cache)
            cache['PINEFORGE_VERSION_SOURCE'] = 'AUTO'
        receipt = {
            'schemaVersion': 'pineforge-settlement-abi-base/v1',
            'commit': provider['commit'],
            'tree': provider['tree'],
            'archive': 'build/lib/libpineforge.a',
            'archiveSha256': identity(archive)['sha256'],
            'headers': headers.name,
            'headersSha256': identity(headers)['sha256'],
            'generatedInclude': 'build/include',
            'generatedHeaderSha256': identity(generated)['sha256'],
            'compiler': compiler,
            'copiedCurrentCache': cache,
        }
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')

    @staticmethod
    def provider_command_name(argv: list[str], suffix: str) -> str:
        for commit, prefix in ((V18_FROZEN_COMMIT, 'v18-frozen-'), (V16_FROZEN_COMMIT, 'v16-frozen-'), (V15_FROZEN_COMMIT, 'v15-frozen-'), (V14_COMMIT, 'v14-'), (V13_COMMIT, 'v13-'),
                               (PRIOR_COMMIT, 'prior-')):
            if commit in argv:
                return prefix + suffix
        return suffix

    def names(self) -> list[str]:
        names = []
        for argv in self.calls:
            if argv[0] == 'cmake' and '--build' in argv and 'ci-smoke' not in ''.join(argv):
                names.append('build')
            elif argv[0] == 'cmake' and '--install' in argv:
                names.append('install')
            elif argv[0] == 'ctest' and '--test-dir' in argv:
                names.append('ctest')
            elif argv[0] == 'git' and 'fetch' in argv:
                names.append(self.provider_command_name(argv, 'fetch'))
            elif any(Path(part).name == 'prepare_settlement_cpp_abi_base.py' for part in argv):
                names.append(self.provider_command_name(argv, 'prepare'))
            elif argv[0] == 'cmake' and '-S' in argv and 'smoke_consumer' in ''.join(argv):
                names.append('smoke-configure')
            elif Path(argv[0]).name == 'smoke_version':
                names.append('smoke-version')
            elif Path(argv[0]).name == 'test_native_live_websocket':
                names.append('require-websocket')
        return names


def run_driver(profile: str, build_dir: Path, scripted: Scripted, extra: list[str] | None = None,
               which=shutil.which) -> int:
    argv = [profile, '--build-dir', str(build_dir), '--jobs', '2', *(extra or [])]
    return main(argv, runner=scripted, stream_output=False, which=which)


def load_summary(build_dir: Path) -> dict:
    return json.loads((build_dir / 'ci-summary.json').read_text())


def stage_names(summary: dict) -> list[str]:
    return [stage['name'] for stage in summary['stages']]


def failure_stages(summary: dict) -> list[str]:
    return [item['stage'] for item in summary['failures']]


class ConfigValidation(unittest.TestCase):
    def test_unknown_profile(self):
        with self.assertRaises(ConfigError):
            parse_args(['relaxed'])
        self.assertEqual(main(['relaxed']), 2)

    def test_unknown_argument_is_rejected(self):
        with self.assertRaises(ConfigError):
            parse_args(['release', '--skip-tests'])
        self.assertEqual(main(['release', '--skip-tests']), 2)

    def test_jobs_range(self):
        with self.assertRaisesRegex(ConfigError, '--jobs'):
            validate_config(parse_args(['release', '--jobs', '0']))
        with self.assertRaisesRegex(ConfigError, '--jobs'):
            validate_config(parse_args(['release', '--jobs', '99']))
        self.assertEqual(main(['release', '--jobs', '0']), 2)

    def test_require_websocket_only_native(self):
        with self.assertRaisesRegex(ConfigError, 'native'):
            validate_config(parse_args(['release', '--require-websocket']))
        self.assertEqual(main(['debug', '--require-websocket']), 2)

    def test_exclude_label_is_a_simple_ctest_label(self):
        cfg = validate_config(parse_args(
            ['release', '--build-dir', 'build-ci-x', '--exclude-label', 'l4-pending']))
        self.assertEqual(cfg.exclude_label, 'l4-pending')
        with self.assertRaisesRegex(ConfigError, 'exclude-label'):
            validate_config(parse_args(['release', '--exclude-label', 'bad label']))

    def test_ccache_requires_installed_tool(self):
        args = parse_args(['release', '--ccache'])
        with self.assertRaisesRegex(ConfigError, 'ccache'):
            validate_config(args, which=which_with_ccache(None))
        self.assertEqual(main(['release', '--ccache'], which=which_with_ccache(None)), 2)

    def test_ccache_binds_compiler_launchers(self):
        launcher = '/usr/bin/true'
        cfg = validate_config(parse_args(['release', '--ccache', '--build-dir', 'build-ci-x']),
                              which=which_with_ccache(launcher))
        values = cmake_cache_definitions(cfg)
        resolved = str(Path(launcher).resolve())
        self.assertEqual(values['CMAKE_C_COMPILER_LAUNCHER'], resolved)
        self.assertEqual(values['CMAKE_CXX_COMPILER_LAUNCHER'], resolved)
        self.assertEqual(values['PINEFORGE_VERSION_SOURCE'], 'FILE')

    def test_curl_dir_must_exist(self):
        missing = ROOT / 'definitely-missing-curl-dir'
        with self.assertRaisesRegex(ConfigError, 'curl-dir'):
            validate_config(parse_args(['native', '--curl-dir', str(missing)]))

    def test_default_build_dirs_are_profile_isolated(self):
        for name in ('release', 'debug', 'sanitizers', 'native', 'kernel'):
            self.assertEqual(default_build_dir(ROOT, name), ROOT / f'build-ci-{name}')
            args = parse_args([name], source=ROOT)
            self.assertEqual(Path(args.build_dir), ROOT / f'build-ci-{name}')

    def test_build_dir_cannot_be_source_root(self):
        with self.assertRaisesRegex(ConfigError, 'source root'):
            validate_config(parse_args(['release', '--build-dir', str(ROOT)]))


class RowFloorConfig(unittest.TestCase):
    def test_min_tests_must_be_a_positive_integer(self):
        for value in ('0', '-3'):
            with self.subTest(value=value):
                with self.assertRaises(ConfigError):
                    validate_config(parse_args(['kernel', '--min-tests', value]))
        with self.assertRaises(ConfigError):
            parse_args(['kernel', '--min-tests', 'many'])
        self.assertEqual(main(['kernel', '--min-tests', '0']), 2)

    def test_kernel_and_release_carry_a_row_floor_and_the_others_do_not(self):
        # expectation corrected: release min_tests None -> RELEASE_MIN_TESTS,
        # because lane P7 gives the release profile a row floor (a row that
        # left release alone failed nothing).
        for name, floor in (('kernel', KERNEL_MIN_TESTS), ('release', RELEASE_MIN_TESTS)):
            with self.subTest(profile=name):
                self.assertEqual(ci_verify.PROFILE[name].min_tests, floor)
                self.assertEqual(validate_config(parse_args([name])).min_tests, floor)
                self.assertEqual(
                    validate_config(parse_args([name, '--min-tests', '9'])).min_tests, 9)
        for name in ('debug', 'sanitizers', 'native'):
            with self.subTest(profile=name):
                self.assertIsNone(ci_verify.PROFILE[name].min_tests)
                self.assertIsNone(validate_config(parse_args([name])).min_tests)
                self.assertEqual(
                    validate_config(parse_args([name, '--min-tests', '9'])).min_tests, 9)

    def test_ctest_row_count_reads_the_closing_summary(self):
        # CTest omits the failed clause when nothing failed (ctest 4.4 prints
        # '100% tests passed out of N'); older summaries spell it out.
        self.assertEqual(ctest_row_count(b'100% tests passed out of 164\n'), 164)
        self.assertEqual(ctest_row_count(b'100% tests passed, 0 tests failed out of 164\n'), 164)
        self.assertEqual(ctest_row_count(b'0% tests passed, 1 tests failed out of 1\n'), 1)
        self.assertEqual(ctest_row_count(
            b'98% tests passed, 3 tests failed out of 158\n\nThe following tests FAILED:\n'), 158)
        self.assertIsNone(ctest_row_count(b'No tests were found!!!\n'))
        self.assertIsNone(ctest_row_count(b''))

    def test_ctest_rows_reads_a_real_ctest_run(self):
        # The installed CTest over a hand-written inventory, so the reader is
        # pinned to CTest's own output rather than a transcription of it. Three
        # rows run (pass, fail, abort), two skip (return code, regex), one
        # cannot start and one is disabled. CTest's own count, 6, includes
        # the skipped and the not-run rows; 3 ran.
        python = sys.executable
        inventory = '\n'.join((
            f'add_test(ran_pass "{python}" "-c" "raise SystemExit(0)")',
            f'add_test(ran_fail "{python}" "-c" "raise SystemExit(3)")',
            f'add_test(ran_abort "{python}" "-c" "__import__(\'os\').abort()")',
            f'add_test(skip_code "{python}" "-c" "raise SystemExit(77)")',
            'set_tests_properties(skip_code PROPERTIES SKIP_RETURN_CODE 77)',
            f'add_test(skip_regex "{python}" "-c" "print(\'SKIP ME\')")',
            'set_tests_properties(skip_regex PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP ME")',
            f'add_test(disabled_row "{python}" "-c" "raise SystemExit(0)")',
            'set_tests_properties(disabled_row PROPERTIES DISABLED TRUE)',
            'add_test(not_run_row "/nonexistent/ci-verify-missing-executable")',
        ))
        with tempfile.TemporaryDirectory() as temporary:
            (Path(temporary) / 'CTestTestfile.cmake').write_text(inventory + '\n')
            result = default_runner(
                ['ctest', '--test-dir', temporary, '--output-on-failure', '--no-tests=error',
                 '--parallel', '2'], timeout=120, stream_output=False)
        output = result.stdout.decode('utf-8', 'replace')
        self.assertNotEqual(result.returncode, 0, output)
        rows = ctest_rows(result.stdout)
        self.assertEqual(ctest_row_count(result.stdout), 6, output)
        self.assertEqual(rows.total, 6, output)
        self.assertEqual(rows.ran, 3, output)
        self.assertEqual(sorted(rows.skipped), ['skip_code', 'skip_regex'])
        self.assertEqual(rows.not_run, ('not_run_row',))
        self.assertEqual(rows.disabled, ('disabled_row',))

    def test_ctest_rows_counts_failed_rows_as_run_and_names_the_rest(self):
        rows = ctest_rows(b'100% tests passed out of 164\n')
        self.assertEqual((rows.total, rows.ran, rows.not_counted()), (164, 164, ''))
        rows = ctest_rows(b'97% tests passed, 1 tests failed out of 30\n\n'
                          b'The following tests FAILED:\n\t  5 - test_slow (Timeout)\n')
        self.assertEqual((rows.total, rows.ran, rows.not_run), (30, 30, ()))
        rows = ctest_rows(ctest_output(9, skipped=['a', 'b'], not_run=['c']))
        self.assertEqual((rows.total, rows.ran), (9, 6))
        self.assertEqual(rows.not_counted(), '; not counted: 2 skipped (a, b), 1 not run (c)')
        self.assertIsNone(ctest_rows(b'No tests were found!!!\n'))

    def test_ctest_rows_fails_closed_on_lists_it_cannot_read(self):
        # A failed count the FAILED list does not match, a Skipped result line
        # the did-not-run list does not name (or the reverse), more rows not
        # run than CTest counted: each raises, so the floor refuses the run
        # rather than guess which rows ran.
        unreadable = (
            b'98% tests passed, 3 tests failed out of 158\n\nThe following tests FAILED:\n',
            b'  1/5 Test  #1: a .....***Skipped   0.01 sec\n\n100% tests passed out of 5\n',
            b'100% tests passed out of 5\n\nThe following tests did not run:\n\t  1 - a (Skipped)\n',
            b'  1/1 Test #1: a ...***Skipped   0.01 sec\n  2/1 Test #2: b ...***Skipped   0.01 sec\n'
            b'\n100% tests passed out of 1\n\nThe following tests did not run:\n'
            b'\t  1 - a (Skipped)\n\t  2 - b (Skipped)\n',
        )
        for output in unreadable:
            with self.subTest(output=output):
                with self.assertRaises(ValueError):
                    ctest_rows(output)


class ProfileOptions(unittest.TestCase):
    def definitions(self, profile: str, extra=None, which=shutil.which):
        args = parse_args([profile, '--build-dir', 'tmp-build', *(extra or [])], source=ROOT)
        cfg = validate_config(args, which=which)
        return cmake_cache_definitions(cfg), cmake_configure_argv(cfg)

    def test_release_keeps_tutorial_on_native_off(self):
        values, argv = self.definitions('release')
        self.assertEqual(values['CMAKE_BUILD_TYPE'], 'Release')
        self.assertEqual(values['PINEFORGE_BUILD_TUTORIAL'], 'ON')
        self.assertEqual(values['PINEFORGE_BUILD_LIVE_RUNNER'], 'OFF')
        self.assertEqual(values['PINEFORGE_ENABLE_SANITIZERS'], 'OFF')
        self.assertEqual(values['PINEFORGE_BUILD_TESTS'], 'ON')
        self.assertEqual(values['PINEFORGE_REQUIRE_ABI_RECEIPTS'], 'ON')
        self.assertEqual(values['PINEFORGE_VERSION_SOURCE'], 'FILE')
        self.assertIn('-DPINEFORGE_VERSION_SOURCE=FILE', argv)
        self.assertNotIn('AUTO', ''.join(argv))

    def test_debug_explicit(self):
        values, _ = self.definitions('debug')
        self.assertEqual(values['CMAKE_BUILD_TYPE'], 'Debug')
        self.assertEqual(values['PINEFORGE_BUILD_TUTORIAL'], 'ON')
        self.assertEqual(values['PINEFORGE_BUILD_LIVE_RUNNER'], 'OFF')
        self.assertEqual(values['PINEFORGE_ENABLE_SANITIZERS'], 'OFF')

    def test_sanitizers_public_flag_expected_and_native_off(self):
        values, argv = self.definitions('sanitizers')
        self.assertEqual(values['CMAKE_BUILD_TYPE'], 'Debug')
        self.assertEqual(values['PINEFORGE_ENABLE_SANITIZERS'], 'ON')
        self.assertEqual(values['PINEFORGE_BUILD_LIVE_RUNNER'], 'OFF')
        self.assertEqual(values['PINEFORGE_BUILD_TUTORIAL'], 'ON')
        self.assertIn('-DPINEFORGE_ENABLE_SANITIZERS=ON', argv)

    def test_kernel_drops_the_source_layer_and_keeps_the_live_runner(self):
        values, argv = self.definitions('kernel')
        self.assertEqual(values['CMAKE_BUILD_TYPE'], 'Release')
        self.assertEqual(values['PINEFORGE_BUILD_SOURCE_LAYER'], 'OFF')
        self.assertEqual(values['PINEFORGE_BUILD_LIVE_RUNNER'], 'ON')
        self.assertEqual(values['PINEFORGE_BUILD_TUTORIAL'], 'OFF')
        self.assertEqual(values['PINEFORGE_ENABLE_SANITIZERS'], 'OFF')
        self.assertIn('-DPINEFORGE_BUILD_SOURCE_LAYER=OFF', argv)

    def test_examples_build_in_release_and_kernel_only(self):
        # Before audit lane N4 every profile pinned PINEFORGE_BUILD_EXAMPLES=OFF,
        # so the example_* ctest rows never ran in any gate.
        for name in ('release', 'kernel'):
            with self.subTest(profile=name):
                values, argv = self.definitions(name)
                self.assertEqual(values['PINEFORGE_BUILD_EXAMPLES'], 'ON')
                self.assertIn('-DPINEFORGE_BUILD_EXAMPLES=ON', argv)
        for name in ('debug', 'sanitizers', 'native'):
            with self.subTest(profile=name):
                values, _ = self.definitions(name)
                self.assertEqual(values['PINEFORGE_BUILD_EXAMPLES'], 'OFF')

    def test_every_other_profile_keeps_the_source_layer(self):
        for name in ('release', 'debug', 'sanitizers', 'native'):
            with self.subTest(profile=name):
                values, _ = self.definitions(name)
                self.assertEqual(values['PINEFORGE_BUILD_SOURCE_LAYER'], 'ON')

    def test_native_live_on_tutorial_off(self):
        curl = tempfile.TemporaryDirectory()
        self.addCleanup(curl.cleanup)
        values, argv = self.definitions(
            'native', extra=['--generator', 'Ninja', '--curl-dir', curl.name, '--require-websocket'])
        self.assertEqual(values['PINEFORGE_BUILD_LIVE_RUNNER'], 'ON')
        self.assertEqual(values['PINEFORGE_BUILD_TUTORIAL'], 'OFF')
        self.assertEqual(values['PINEFORGE_ENABLE_SANITIZERS'], 'OFF')
        self.assertEqual(values['CURL_DIR'], str(Path(curl.name).resolve()))
        self.assertIn('-G', argv)
        self.assertIn('Ninja', argv)


class RuntimeBudgetLanes(unittest.TestCase):
    """Every Release lane of the CI build matrix runs the relative runtime gate.

    A Release configure whose environment sets
    PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY=1 registers test_l4g_runtime_budget
    --candidate-only: one correctness sample, no ratio. A40 rev 6 set it for the
    hosted macOS lane because wall clock was no stable timing host there; since
    A40 rev 7 (lane Q9) the gated sample is process CPU time, and no lane is
    exempt. Setting it again is the escape for a runner whose CPU accounting is
    unusable, and needs its reason in ci.yml, in docs/ci.md and here.
    """
    WORKFLOW = ROOT / '.github/workflows/ci.yml'
    SETTING = re.compile(r'^\s*PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY:(.*)$', re.MULTILINE)

    def test_no_ci_lane_registers_the_runtime_budget_candidate_only(self):
        values = [value.strip().strip('\'"')
                  for value in self.SETTING.findall(self.WORKFLOW.read_text())]
        self.assertEqual([value for value in values if value != '0'], [],
                         'a ci.yml job sets PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY to other than '
                         '0, so a Release lane registers test_l4g_runtime_budget --candidate-only')

    def test_the_escape_the_workflow_names_still_reaches_the_row(self):
        # The ci.yml comment offers the variable as the escape; it only is one
        # while the row's registration still reads it.
        self.assertRegex((ROOT / 'tests/CMakeLists.txt').read_text(),
                         r'"\$ENV\{PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY\}" STREQUAL "1"')


class CopyCacheIdentity(unittest.TestCase):
    def test_copy_cache_includes_version_source_and_launchers(self):
        self.assertIn('PINEFORGE_VERSION_SOURCE', COPY_CACHE)
        self.assertIn('CMAKE_C_COMPILER_LAUNCHER', COPY_CACHE)
        self.assertIn('CMAKE_CXX_COMPILER_LAUNCHER', COPY_CACHE)

    def test_receipt_mismatch_on_version_source(self):
        compiler = {'target': 't', 'sha256': 'abc', 'version': 'cc'}
        receipt = {
            'schemaVersion': 'pineforge-settlement-abi-base/v1',
            'commit': BASE_COMMIT,
            'tree': BASE_TREE,
            'compiler': compiler,
            'copiedCurrentCache': {'PINEFORGE_VERSION_SOURCE': 'AUTO'},
        }
        with self.assertRaisesRegex(RuntimeError, 'version-source/launcher'):
            receipt_matches_current(receipt, {'PINEFORGE_VERSION_SOURCE': 'FILE'}, compiler)

    def test_receipt_match_on_identical_identity(self):
        compiler = {'target': 't', 'sha256': 'abc', 'version': 'cc'}
        cache = {'PINEFORGE_VERSION_SOURCE': 'FILE', 'CMAKE_CXX_COMPILER_LAUNCHER': '/ccache'}
        receipt = {
            'schemaVersion': 'pineforge-settlement-abi-base/v1',
            'commit': BASE_COMMIT,
            'tree': BASE_TREE,
            'compiler': compiler,
            'copiedCurrentCache': copied_cache(cache),
        }
        receipt_matches_current(receipt, cache, compiler)


class HistoricalProviderPins(unittest.TestCase):
    def test_v18_frozen_preparation_uses_the_fc7aad6_header_closure(self):
        provider = PROVIDERS['v18-frozen']
        self.assertEqual(provider['commit'], V18_FROZEN_COMMIT)
        self.assertEqual(provider['tree'], V18_FROZEN_TREE)
        self.assertEqual(provider['manifest'],
                         ROOT / 'tests/fixtures/native_cpp_abi/host-fc7aad6/manifest.json')
        self.assertEqual(provider['default_output'], 'native-abi-v18-frozen')
        self.assertEqual(provider['headers_name'], 'headers.tar')
        archive = provider['manifest'].parent / provider['headers_name']
        self.assertEqual(identity(archive)['sha256'],
                         '91f93ac68ff072b2e1977fe748a29f2ae47901fb76e13461d90b91b66229de1b')

    def test_v16_frozen_preparation_uses_the_ab9714b_header_closure(self):
        provider = PROVIDERS['v16-frozen']
        self.assertEqual(provider['commit'], V16_FROZEN_COMMIT)
        self.assertEqual(provider['tree'], V16_FROZEN_TREE)
        self.assertEqual(provider['manifest'],
                         ROOT / 'tests/fixtures/native_cpp_abi/host-ab9714b/manifest.json')
        self.assertEqual(provider['default_output'], 'native-abi-v16-frozen')
        self.assertEqual(provider['headers_name'], 'headers.tar')
        archive = provider['manifest'].parent / provider['headers_name']
        self.assertEqual(identity(archive)['sha256'],
                         '1a1ab85239ce1bca9022f879ecc0e88c2ee0af719c74cdfe9d8e9d5aaada8d98')

    def test_v15_frozen_preparation_uses_the_e7cdf052_header_closure(self):
        provider = PROVIDERS['v15-frozen']
        self.assertEqual(provider['commit'], V15_FROZEN_COMMIT)
        self.assertEqual(provider['tree'], V15_FROZEN_TREE)
        self.assertEqual(provider['manifest'],
                         ROOT / 'tests/fixtures/native_cpp_abi/host-e7cdf05/manifest.json')
        self.assertEqual(provider['default_output'], 'native-abi-v15-frozen')
        self.assertEqual(provider['headers_name'], 'headers.tar')
        archive = provider['manifest'].parent / provider['headers_name']
        self.assertEqual(identity(archive)['sha256'],
                         '189a0e99ff60f7c9284243117fe501ebf9a9fb6269c787dad35957d0ca7a6ed3')

    def test_v14_preparation_uses_the_frozen_f736676_header_closure(self):
        provider = PROVIDERS['v14']
        self.assertEqual(provider['commit'], V14_COMMIT)
        self.assertEqual(provider['tree'], V14_TREE)
        self.assertEqual(provider['manifest'],
                         ROOT / 'tests/fixtures/native_cpp_abi/host-f736676/manifest.json')
        self.assertEqual(provider['default_output'], 'native-abi-v14')
        self.assertEqual(provider['headers_name'], 'headers.tar')
        archive = provider['manifest'].parent / provider['headers_name']
        self.assertEqual(identity(archive)['sha256'],
                         '37e9340e0a985db118006e7e3b265e0191445285ce5e8fd8fc77f1578275e28e')

    def test_all_historical_roles_pin_their_own_engine_epoch(self):
        self.assertEqual({role: provider['engine_epoch'] for role, provider in PROVIDERS.items()}, {
            'e60': 'engine_script_run_v13', '0e': 'engine_script_run_v13',
            'v13': 'engine_script_run_v13', 'v14': 'engine_script_run_v14',
            'v15-frozen': 'engine_script_run_v15',
            'v16-frozen': 'engine_script_run_v16',
            'v18-frozen': 'engine_script_run_v18',
        })

    def test_host_provider_epoch_matches_its_authenticated_header_owner(self):
        for role in ('v13', 'v14', 'v15-frozen', 'v16-frozen', 'v18-frozen'):
            provider = PROVIDERS[role]
            with self.subTest(role=role), tempfile.TemporaryDirectory() as temporary:
                source = Path(temporary) / 'headers'
                extract_tar((provider['manifest'].parent / provider['headers_name']).read_bytes(), source)
                authenticate_headers(source, provider['manifest'],
                                     commit=provider['commit'], tree=provider['tree'])
                epochs = set(re.findall(r'inline namespace (engine_script_run_v\d+)',
                                        (source / 'include/pineforge/engine.hpp').read_text()))
                self.assertEqual(epochs, {provider['engine_epoch']})

    def test_v14_header_authentication_rejects_changed_missing_and_extra_files(self):
        provider = PROVIDERS['v14']
        raw = (provider['manifest'].parent / provider['headers_name']).read_bytes()
        for mutation in ('changed', 'missing', 'extra'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                source = Path(temporary) / 'headers'
                extract_tar(raw, source)
                header = source / 'include/pineforge/native_host.hpp'
                if mutation == 'changed':
                    header.write_bytes(header.read_bytes() + b'\n// changed\n')
                    message = 'header bytes differ'
                elif mutation == 'missing':
                    header.unlink()
                    message = 'header closure differs'
                else:
                    (source / 'include/pineforge/untracked.hpp').write_text('// extra\n')
                    message = 'header closure differs'
                with self.assertRaisesRegex(RuntimeError, message):
                    authenticate_headers(source, provider['manifest'],
                                         commit=V14_COMMIT, tree=V14_TREE)

    def test_v14_header_authentication_rejects_wrong_commit_or_tree(self):
        provider = PROVIDERS['v14']
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'headers'
            extract_tar((provider['manifest'].parent / provider['headers_name']).read_bytes(), source)
            for commit, tree in ((V13_COMMIT, V14_TREE), (V14_COMMIT, V13_TREE)):
                with self.subTest(commit=commit, tree=tree):
                    with self.assertRaisesRegex(RuntimeError, 'header pin changed'):
                        authenticate_headers(source, provider['manifest'], commit=commit, tree=tree)


class CAbiRuntimeInventory(unittest.TestCase):
    def test_public_and_runtime_export_counts_are_exact(self):
        from check_c_abi_runtime import (
            EXPECTED_PUBLIC_DECLARATIONS,
            EXPECTED_RUNTIME,
            EXPECTED_RUNTIME_IMPLEMENTATIONS,
            _pf_api_names,
        )
        header = _pf_api_names(ROOT / 'include/pineforge/pineforge.h')
        runtime = _pf_api_names(ROOT / 'src/c_abi.cpp')
        self.assertEqual(EXPECTED_PUBLIC_DECLARATIONS, 65)
        self.assertEqual(EXPECTED_RUNTIME_IMPLEMENTATIONS, 57)
        self.assertEqual(len(EXPECTED_RUNTIME), EXPECTED_RUNTIME_IMPLEMENTATIONS)
        self.assertEqual(len(header), EXPECTED_PUBLIC_DECLARATIONS)
        self.assertEqual(len(header), len(set(header)))
        self.assertEqual(len(runtime), EXPECTED_RUNTIME_IMPLEMENTATIONS)
        self.assertEqual(set(runtime), EXPECTED_RUNTIME)
        self.assertIn('strategy_configure_native_fx_curve_v1', header)
        self.assertIn('strategy_configure_native_fx_curve_v1', runtime)


class ReceiptReuse(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.build = self.root / 'build'
        self.output = self.build / 'settlement-abi-base'
        self.cxx = shutil.which('c++') or '/usr/bin/c++'
        self.output.mkdir(parents=True)
        (self.output / 'sentinel').write_text('keep\n')

    def tearDown(self):
        self.temporary.cleanup()

    def seed(self, *, version='FILE', corrupt=False, receipt=True, prior=False, provider=None):
        pin = PROVIDERS[provider or ('0e' if prior else 'e60')]
        archive = self.output / 'build' / 'lib' / 'libpineforge.a'
        headers = self.output / pin['headers_name']
        generated = self.output / 'build' / 'include' / 'pineforge' / 'version.h'
        archive.parent.mkdir(parents=True, exist_ok=True)
        generated.parent.mkdir(parents=True, exist_ok=True)
        archive.write_bytes(b'!<arch>\nbase\n')
        headers.write_bytes(b'headers')
        generated.write_text('version\n')
        write_cache(self.build / 'CMakeCache.txt', {
            'CMAKE_CXX_COMPILER': self.cxx,
            'PINEFORGE_VERSION_SOURCE': 'FILE',
            'CMAKE_BUILD_TYPE': 'Release',
        })
        if not receipt:
            return
        current = copied_cache(read_cache_for_test(self.build / 'CMakeCache.txt'))
        payload = {
            'schemaVersion': 'pineforge-settlement-abi-base/v1',
            'commit': pin['commit'],
            'tree': pin['tree'],
            'archive': 'build/lib/libpineforge.a',
            'archiveSha256': identity(archive)['sha256'],
            'headers': headers.name,
            'headersSha256': identity(headers)['sha256'],
            'generatedInclude': 'build/include',
            'generatedHeaderSha256': identity(generated)['sha256'],
            'compiler': compiler_identity(self.cxx),
            'copiedCurrentCache': dict(current),
        }
        if version != 'FILE':
            payload['copiedCurrentCache']['PINEFORGE_VERSION_SOURCE'] = version
        if corrupt:
            payload['archiveSha256'] = '0' * 64
        (self.output / 'receipt.json').write_text(json.dumps(payload) + '\n')

    def test_matching_receipt_is_reused(self):
        self.seed()
        receipt = reusable_prepared_base(self.output, self.build)
        self.assertEqual(receipt['commit'], BASE_COMMIT)
        self.assertTrue((self.output / 'sentinel').is_file())

    def test_mismatch_refuses_without_deletion(self):
        self.seed(version='AUTO')
        with self.assertRaisesRegex(RuntimeError, 'fresh --build-dir'):
            reusable_prepared_base(self.output, self.build)
        self.assertTrue((self.output / 'sentinel').is_file())
        self.assertTrue((self.output / 'receipt.json').is_file())

    def test_missing_receipt_refuses_without_deletion(self):
        self.seed(receipt=False)
        with self.assertRaisesRegex(RuntimeError, 'fresh --build-dir'):
            reusable_prepared_base(self.output, self.build)
        self.assertTrue((self.output / 'sentinel').is_file())

    def test_corrupt_archive_hash_refuses_without_deletion(self):
        self.seed(corrupt=True)
        with self.assertRaisesRegex(RuntimeError, 'fresh --build-dir'):
            reusable_prepared_base(self.output, self.build)
        self.assertTrue((self.output / 'sentinel').is_file())

    def test_prior_reuse_pins_its_own_commit_and_tree(self):
        self.seed(prior=True)
        receipt = reusable_prepared_base(self.output, self.build, commit=PRIOR_COMMIT, tree=PRIOR_TREE)
        self.assertEqual(receipt['commit'], PRIOR_COMMIT)
        with self.assertRaisesRegex(RuntimeError, 'does not pin e60 R2'):
            reusable_prepared_base(self.output, self.build)
        with self.assertRaisesRegex(RuntimeError, 'does not pin 0e18690'):
            reusable_prepared_base(self.output, self.build, commit=PRIOR_COMMIT, tree=BASE_TREE)
        self.assertTrue((self.output / 'sentinel').is_file())

    def test_prior_reuse_also_refuses_version_source_mismatch(self):
        self.seed(prior=True, version='AUTO')
        with self.assertRaisesRegex(RuntimeError, 'fresh --build-dir'):
            reusable_prepared_base(self.output, self.build, commit=PRIOR_COMMIT, tree=PRIOR_TREE)
        self.assertTrue((self.output / 'sentinel').is_file())

    def test_v14_reuse_pins_its_own_commit_and_tree(self):
        self.seed(provider='v14')
        receipt = reusable_prepared_base(self.output, self.build, commit=V14_COMMIT, tree=V14_TREE)
        self.assertEqual(receipt['commit'], V14_COMMIT)
        self.assertEqual(receipt['headers'], 'headers.tar')
        for commit, tree in ((V13_COMMIT, V14_TREE), (V14_COMMIT, V13_TREE)):
            with self.subTest(commit=commit, tree=tree):
                with self.assertRaisesRegex(RuntimeError, 'does not pin'):
                    reusable_prepared_base(self.output, self.build, commit=commit, tree=tree)
        self.assertTrue((self.output / 'sentinel').is_file())

    def test_v14_reuse_refuses_changed_header_or_generated_header_bytes(self):
        for relative in ('headers.tar', 'build/include/pineforge/version.h'):
            with self.subTest(relative=relative):
                self.seed(provider='v14')
                (self.output / relative).write_bytes(b'changed')
                with self.assertRaisesRegex(RuntimeError, 'fresh --build-dir'):
                    reusable_prepared_base(self.output, self.build, commit=V14_COMMIT, tree=V14_TREE)
                self.assertTrue((self.output / 'sentinel').is_file())


def read_cache_for_test(path: Path) -> dict[str, str]:
    from prepare_settlement_cpp_abi_base import read_cache
    return read_cache(path)


class GitObjectDiscovery(unittest.TestCase):
    def test_real_object_lookup_uses_the_supplied_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            def git(*args):
                result = default_runner(['git', '-C', temporary, *args], stream_output=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout.decode().strip()
            git('init', '--quiet')
            self.assertFalse(ci_verify.pinned_object_present(source, default_runner, '0' * 40))
            git('-c', 'user.name=CI fixture', '-c', 'user.email=ci@example.invalid',
                '-c', 'commit.gpgsign=false', 'commit', '--quiet', '--allow-empty', '-m', 'fixture')
            commit = git('rev-parse', 'HEAD')
            self.assertTrue(ci_verify.pinned_object_present(source, default_runner, commit))
            self.assertFalse(ci_verify.pinned_object_present(source, default_runner, BASE_COMMIT))

    def test_scripted_inventory_does_not_read_ambient_git_history(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)  # deliberately not a Git repository
            scripted = Scripted(source / 'build', source)
            for commit in (BASE_COMMIT, PRIOR_COMMIT, V13_COMMIT, V14_COMMIT,
                           V15_FROZEN_COMMIT, V16_FROZEN_COMMIT, V18_FROZEN_COMMIT):
                self.assertTrue(ci_verify.pinned_object_present(source, scripted, commit))
            self.assertFalse(ci_verify.pinned_object_present(source, scripted, '0' * 40))


class DriverOrderingAndAggregation(unittest.TestCase):
    def test_configure_binds_invoking_python_without_resolving_venv_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            entry = Path(temporary) / 'venv' / 'bin' / 'python'
            entry.parent.mkdir(parents=True)
            entry.symlink_to(sys.executable)
            with mock.patch.object(ci_verify.sys, 'executable', str(entry)):
                cfg = ci_verify.build_config(['release', '--build-dir', str(Path(temporary) / 'build')])
                definitions = cmake_cache_definitions(cfg)
                self.assertEqual(definitions.get('Python3_EXECUTABLE'), str(entry))
                self.assertNotEqual(definitions['Python3_EXECUTABLE'], str(entry.resolve()))

    def test_different_configured_python_refuses_before_build(self):
        code, summary, scripted, _ = self.run_profile(cache_python='/not-the-invoking-python')
        self.assertEqual(code, 1)
        self.assertIn('profile-options', failure_stages(summary))
        self.assertNotIn('build', scripted.names())
        self.assertIn('Python3_EXECUTABLE', summary['failures'][0]['error'])

    def test_empty_inventory_fails_real_ctest_without_hiding_package_checks(self):
        code, summary, scripted, build_dir = self.run_profile(actual_empty_ctest=True)
        self.assertEqual(code, 1)
        self.assertIn('ctest', failure_stages(summary))
        self.assertIn('install', scripted.names())
        self.assertIn('smoke-version', scripted.names())
        self.assertIn('No tests were found', (build_dir / 'ci-logs/ctest.log').read_text())
        ctest = next(argv for argv in scripted.calls if argv[0] == 'ctest' and '--test-dir' in argv)
        self.assertIn('--no-tests=error', ctest)

    def test_real_empty_ctest_default_succeeds_but_strict_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            (Path(temporary) / 'CTestTestfile.cmake').write_text('# deliberately empty inventory\n')
            argv = ['ctest', '--test-dir', temporary]
            default = default_runner(argv, stream_output=False)
            strict = default_runner([*argv, '--no-tests=error'], stream_output=False)
        self.assertEqual(default.returncode, 0, default.stdout)
        self.assertNotEqual(strict.returncode, 0)
        self.assertIn(b'No tests were found', default.stdout + default.stderr)
        self.assertIn(b'No tests were found', strict.stdout + strict.stderr)

    def test_local_ccache_uses_content_identity(self):
        seen = []
        def runner(argv, **kwargs):
            seen.append(kwargs["extra_env"])
            return Completed(0)
        with tempfile.TemporaryDirectory() as temporary:
            cfg = ci_verify.build_config(
                ["release", "--build-dir", temporary, "--ccache"], runner=runner,
                stream_output=False, which=lambda name: "/usr/bin/" + name)
            driver = ci_verify.Driver(cfg)
            driver.invoke("probe", ["compile"], extra_env={"PRESERVED_SETTING": "yes"})
        self.assertEqual(seen, [{"PRESERVED_SETTING": "yes", "CCACHE_COMPILERCHECK": "content"}])

    def test_runner_type_error_is_not_retried(self):
        calls = []
        def failing_runner(argv, **kwargs):
            calls.append(argv)
            raise TypeError("runner failure after dispatch")
        with self.assertRaisesRegex(TypeError, "after dispatch"):
            ci_verify.call_runner(failing_runner, ["build-once"])
        self.assertEqual(calls, [["build-once"]])

    def run_profile(self, profile='release', extra=None, **exits):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        build_dir = Path(temporary.name) / 'build'
        scripted = Scripted(build_dir, ROOT, profile, **exits)
        code = run_driver(profile, build_dir, scripted, extra=extra)
        summary = load_summary(build_dir)
        return code, summary, scripted, build_dir

    def test_source_guards_are_real_and_pass_on_this_tree(self):
        for name, argv in source_guard_commands(ROOT):
            with self.subTest(name=name):
                result = default_runner(argv, extra_env=None, timeout=120,
                                        combine_stderr=True, stream_output=False)
                self.assertEqual(result.returncode, 0, result.stdout[-2000:])

    def test_corpus_submodule_is_initialized_before_source_guards(self):
        code, summary, scripted, _ = self.run_profile()
        self.assertEqual(code, 0, summary['failures'])
        names = stage_names(summary)
        self.assertLess(names.index('corpus-submodule-init'),
                        names.index('source-guard-c-abi'))
        self.assertIn('corpus-submodule-pin', names)

    def test_uninitialized_corpus_status_fails_before_configure(self):
        code, summary, scripted, _ = self.run_profile(**{'corpus-submodule-prefix': '-'})
        self.assertEqual(code, 1)
        self.assertIn('corpus-submodule-pin', failure_stages(summary))
        self.assertFalse(any(argv[0] == 'cmake' and '-S' in argv for argv in scripted.calls))

    def test_build_failure_skips_ctest_and_install(self):
        code, summary, scripted, _ = self.run_profile(build=1)
        self.assertEqual(code, 1)
        self.assertIn('build', failure_stages(summary))
        self.assertNotIn('ctest', scripted.names())
        self.assertNotIn('install', scripted.names())
        self.assertNotIn('smoke-version', scripted.names())

    def test_configure_failure_skips_build(self):
        code, summary, scripted, _ = self.run_profile(configure=1)
        self.assertEqual(code, 1)
        self.assertIn('configure', failure_stages(summary))
        self.assertNotIn('build', scripted.names())
        self.assertNotIn('ctest', scripted.names())

    def test_source_guard_failure_skips_configure(self):
        code, summary, scripted, _ = self.run_profile(**{'source-guard-c-abi': 1})
        self.assertEqual(code, 1)
        self.assertIn('source-guard-c-abi', failure_stages(summary))
        self.assertFalse(any(argv[0] == 'cmake' and '-S' in argv for argv in scripted.calls))

    def test_adapter_spec_shadowing_guard_failure_skips_configure(self):
        # R5 N11 registered this check as a CTest row only, so a shadowed spec
        # field surfaced after a full build; as a source guard (lane P7) it
        # fails every profile -- the kernel one included -- and ci_preflight
        # before configure.
        self.assertIn('source-guard-adapter-spec-shadowing',
                      [name for name, _ in source_guard_commands(ROOT)])
        code, summary, scripted, _ = self.run_profile(
            'kernel', **{'source-guard-adapter-spec-shadowing': 1})
        self.assertEqual(code, 1)
        self.assertIn('source-guard-adapter-spec-shadowing', failure_stages(summary))
        self.assertFalse(any(argv[0] == 'cmake' and '-S' in argv for argv in scripted.calls))

    def test_native_include_independence_runs_for_release_native_and_kernel_only(self):
        for profile, expected in (("release", True), ("native", True), ("kernel", True),
                                  ("debug", False), ("sanitizers", False)):
            with self.subTest(profile=profile):
                code, summary, _, _ = self.run_profile(profile)
                self.assertEqual(code, 0, summary['failures'])
                self.assertEqual('native-include-independence' in stage_names(summary), expected)

    def test_kernel_asserts_the_kernel_archive_and_others_do_not(self):
        for profile, expected in (("kernel", True), ("release", False), ("native", False)):
            with self.subTest(profile=profile):
                cfg = validate_config(parse_args([profile, '--build-dir', 'tmp-build']))
                argv = native_include_independence_command(cfg, Path('/tmp/prefix'))
                self.assertEqual('--kernel-archive' in argv, expected)
                if expected:
                    archive = argv[argv.index('--kernel-archive') + 1]
                    self.assertEqual(Path(archive).name, 'libpineforge_kernel.a')

    def test_kernel_residuals_gate_runs_for_the_kernel_profile_only(self):
        for profile, expected in (("kernel", True), ("release", False), ("native", False),
                                  ("debug", False), ("sanitizers", False)):
            with self.subTest(profile=profile):
                code, summary, _, _ = self.run_profile(profile)
                self.assertEqual(code, 0, summary['failures'])
                self.assertEqual('kernel-residuals' in stage_names(summary), expected)

    def test_kernel_residuals_command_names_the_kernel_archive_and_the_adr(self):
        cfg = validate_config(parse_args(['kernel', '--build-dir', 'tmp-build']))
        argv = ci_verify.kernel_residuals_command(cfg)
        self.assertEqual(Path(argv[1]).name, 'check_kernel_residuals.py')
        self.assertEqual(Path(argv[argv.index('--archive') + 1]).name, 'libpineforge_kernel.a')
        self.assertEqual(Path(argv[argv.index('--adr') + 1]).name,
                         '0001-kernel-adapter-boundary.md')

    def test_kernel_residuals_failure_stops_before_ctest(self):
        code, summary, scripted, _ = self.run_profile('kernel', **{'kernel-residuals': 1})
        self.assertEqual(code, 1)
        self.assertIn('kernel-residuals', failure_stages(summary))
        names = stage_names(summary)
        self.assertLess(names.index('native-include-independence'), names.index('kernel-residuals'))
        self.assertNotIn('ctest', scripted.names())

    def test_kernel_skips_the_receipt_backed_abi_providers(self):
        code, summary, scripted, _ = self.run_profile('kernel')
        self.assertEqual(code, 0, summary['failures'])
        self.assertIn('abi-providers-skipped', stage_names(summary))
        self.assertNotIn('abi-base', scripted.names())
        self.assertNotIn('abi-v16-frozen', scripted.names())
        self.assertNotIn('abi-v18-frozen', scripted.names())
        self.assertIn('ctest', scripted.names())

    def test_twin_parity_guard_runs_for_release_and_native_only(self):
        for profile, expected in (("release", True), ("native", True),
                                  ("debug", False), ("sanitizers", False)):
            with self.subTest(profile=profile):
                code, summary, _, _ = self.run_profile(profile)
                self.assertEqual(code, 0, summary['failures'])
                self.assertEqual('source-guard-twin-parity' in stage_names(summary), expected)

    def test_twin_parity_failure_skips_configure(self):
        code, summary, scripted, _ = self.run_profile(**{'source-guard-twin-parity': 1})
        self.assertEqual(code, 1)
        self.assertIn('source-guard-twin-parity', failure_stages(summary))
        self.assertFalse(any(argv[0] == 'cmake' and '-S' in argv for argv in scripted.calls))

    def test_native_include_independence_failure_stops_before_abi(self):
        code, summary, scripted, _ = self.run_profile(**{'native-include-independence': 1})
        self.assertEqual(code, 1)
        self.assertIn('native-include-independence', failure_stages(summary))
        self.assertNotIn('abi-base', scripted.names())
        self.assertNotIn('ctest', scripted.names())

    def test_ctest_and_smoke_failures_are_both_collected(self):
        code, summary, scripted, _ = self.run_profile(ctest=8, **{'smoke-stdout': '9.9.9'})
        self.assertEqual(code, 1)
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())
        self.assertIn('smoke-version', scripted.names())
        self.assertIn('ctest', failure_stages(summary))
        self.assertIn('smoke-version-check', failure_stages(summary))
        message = next(item['error'] for item in summary['failures'] if item['stage'] == 'smoke-version-check')
        self.assertIn(repr(expected_version(ROOT)), message)
        self.assertIn(repr('9.9.9'), message)
        self.assertEqual(summary['expectedVersion'], expected_version(ROOT))
        self.assertEqual(summary['actualVersion'], '9.9.9')

    def test_ctest_failure_does_not_hide_install_failure(self):
        code, summary, scripted, _ = self.run_profile(ctest=8, install=1)
        self.assertEqual(code, 1)
        self.assertIn('ctest', failure_stages(summary))
        self.assertIn('install', failure_stages(summary))
        self.assertNotIn('smoke-version', scripted.names())

    def test_successful_scripted_release_exit_zero(self):
        code, summary, scripted, build_dir = self.run_profile()
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['status'], 'passed')
        self.assertEqual(summary['exitCode'], 0)
        self.assertEqual(summary['versionSource'], 'FILE')
        self.assertIn('abi-base', stage_names(summary))
        self.assertIn('abi-prior', stage_names(summary))
        self.assertIn('abi-v13', stage_names(summary))
        self.assertIn('abi-v14', stage_names(summary))
        self.assertIn('abi-v15-frozen', stage_names(summary))
        self.assertIn('abi-v16-frozen', stage_names(summary))
        self.assertIn('abi-v18-frozen', stage_names(summary))
        names = stage_names(summary)
        self.assertLess(names.index('build'), names.index('abi-base'))
        self.assertLess(names.index('abi-base'), names.index('abi-prior'))
        self.assertLess(names.index('abi-prior'), names.index('ctest'))
        self.assertLess(names.index('abi-prior'), names.index('abi-v13'))
        self.assertLess(names.index('abi-v13'), names.index('abi-v14'))
        self.assertLess(names.index('abi-v14'), names.index('abi-v15-frozen'))
        self.assertLess(names.index('abi-v15-frozen'), names.index('abi-v16-frozen'))
        self.assertLess(names.index('abi-v16-frozen'), names.index('abi-v18-frozen'))
        self.assertLess(names.index('abi-v18-frozen'), names.index('ctest'))
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())
        self.assertTrue((build_dir / 'ci-logs' / 'ctest.log').is_file())
        self.assertTrue((build_dir / 'ci-summary.json').is_file())
        ctest_argv = next(argv for argv in scripted.calls if argv[0] == 'ctest' and '--test-dir' in argv)
        if ctest_supports_junit(scripted):
            self.assertIn('--output-junit', ctest_argv)
            self.assertEqual(Path(ctest_argv[ctest_argv.index('--output-junit') + 1]).resolve(),
                             (build_dir / 'ctest-junit.xml').resolve())

    def test_ctest_label_exclusion_is_forwarded(self):
        code, summary, scripted, _ = self.run_profile(
            extra=['--exclude-label', 'l4-pending'])
        self.assertEqual(code, 0, summary['failures'])
        ctest_argv = next(
            argv for argv in scripted.calls if argv[0] == 'ctest' and '--test-dir' in argv)
        self.assertIn('-LE', ctest_argv)
        self.assertEqual(ctest_argv[ctest_argv.index('-LE') + 1], 'l4-pending')

    def test_junit_flag_omitted_when_unsupported(self):
        code, summary, scripted, build_dir = self.run_profile(junit_help='absent')
        self.assertEqual(code, 0, summary['failures'])
        ctest_argv = next(argv for argv in scripted.calls if argv[0] == 'ctest' and '--test-dir' in argv)
        self.assertNotIn('--output-junit', ctest_argv)

    def test_ctest_label_exclusion_is_forwarded(self):
        code, summary, scripted, _ = self.run_profile(extra=['--exclude-label', 'l4-pending'])
        self.assertEqual(code, 0, summary['failures'])
        ctest_argv = next(
            argv for argv in scripted.calls if argv[0] == 'ctest' and '--test-dir' in argv)
        self.assertIn('-LE', ctest_argv)
        self.assertEqual(ctest_argv[ctest_argv.index('-LE') + 1], 'l4-pending')

    def test_matching_base_is_reused_without_fetch_or_prepare(self):
        code, summary, scripted, _ = self.run_profile(preexisting_base='match')
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abi']['action'], 'reused')
        self.assertNotIn('fetch', scripted.names())
        self.assertNotIn('prepare', scripted.names())

    def test_mismatch_base_refuses_without_deletion_and_still_runs_ctest_install(self):
        code, summary, scripted, build_dir = self.run_profile(preexisting_base='mismatch')
        self.assertEqual(code, 1)
        self.assertEqual(summary['abi']['action'], 'refused')
        self.assertIn('fresh --build-dir', summary['failures'][0]['error'])
        self.assertTrue((build_dir / 'settlement-abi-base' / 'sentinel').is_file())
        self.assertNotIn('prepare', scripted.names())
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())

    def test_fetch_no_tags_only_when_object_missing(self):
        code, summary, scripted, _ = self.run_profile(**{'cat-file': 1})
        self.assertEqual(code, 0, summary['failures'])
        fetch = next(argv for argv in scripted.calls if argv[0] == 'git' and 'fetch' in argv)
        self.assertIn('--no-tags', fetch)
        self.assertNotIn('--tags', fetch)
        self.assertIn(BASE_COMMIT, fetch)
        self.assertIn('prepare', scripted.names())

    def test_present_object_does_not_fetch(self):
        _, _, scripted, _ = self.run_profile()
        self.assertNotIn('fetch', scripted.names())
        self.assertIn('prepare', scripted.names())

    def test_matching_prior_is_reused_without_fetch_or_prepare(self):
        code, summary, scripted, _ = self.run_profile(preexisting_prior='match')
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiPrior']['action'], 'reused')
        self.assertNotIn('prior-fetch', scripted.names())
        self.assertNotIn('prior-prepare', scripted.names())

    def test_mismatch_prior_refuses_without_deletion_and_keeps_other_checks(self):
        code, summary, scripted, build_dir = self.run_profile(preexisting_prior='mismatch')
        self.assertEqual(code, 1)
        self.assertEqual(summary['abiPrior']['action'], 'refused')
        self.assertIn('abi-prior', failure_stages(summary))
        self.assertIn('fresh --build-dir', summary['failures'][0]['error'])
        self.assertTrue((build_dir / 'settlement-abi-prior' / 'sentinel').is_file())
        self.assertTrue((build_dir / 'settlement-abi-prior' / 'receipt.json').is_file())
        self.assertNotIn('prior-prepare', scripted.names())
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())

    def test_prior_fetch_is_exact_and_only_when_object_missing(self):
        code, summary, scripted, build_dir = self.run_profile(**{'prior-cat-file': 1})
        self.assertEqual(code, 0, summary['failures'])
        fetches = [argv for argv in scripted.calls if argv[0] == 'git' and 'fetch' in argv]
        self.assertEqual(fetches, [['git', '-C', str(ROOT), 'fetch', '--no-tags', '--depth=1',
                                    'origin', PRIOR_COMMIT]])
        self.assertIn('abi-prior-fetch', stage_names(summary))
        prepare = next(argv for argv in scripted.calls
                       if any(Path(part).name == 'prepare_settlement_cpp_abi_base.py' for part in argv)
                       and PRIOR_COMMIT in argv)
        self.assertEqual(prepare[prepare.index('--tree') + 1], PRIOR_TREE)
        self.assertEqual(Path(prepare[prepare.index('--output') + 1]).resolve(),
                         (build_dir / 'settlement-abi-prior').resolve())
        self.assertEqual(prepare[prepare.index('--header-manifest') + 1],
                         str(ROOT / 'tests/fixtures/settlement_cpp_abi/0e18690/manifest.json'))

    def test_v13_provider_uses_its_own_pinned_profile_preparation(self):
        code, summary, scripted, build_dir = self.run_profile(**{'v13-cat-file': 1})
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV13']['action'], 'prepared')
        fetches = [argv for argv in scripted.calls if argv[0] == 'git' and 'fetch' in argv]
        self.assertEqual(fetches, [['git', '-C', str(ROOT), 'fetch', '--no-tags', '--depth=1', 'origin', V13_COMMIT]])
        prepare = next(argv for argv in scripted.calls if V13_COMMIT in argv and '--tree' in argv)
        self.assertEqual(prepare[prepare.index('--tree')+1], V13_TREE)
        self.assertEqual(Path(prepare[prepare.index('--output')+1]).resolve(), (build_dir/'native-abi-v13').resolve())
        self.assertEqual(prepare[prepare.index('--header-manifest')+1],
                         str(ROOT/'tests/fixtures/native_cpp_abi/host-c3ed455/manifest.json'))

    def test_v13_provider_failure_is_reported_without_hiding_checks(self):
        code, summary, scripted, _ = self.run_profile(**{'v13-prepare': 1})
        self.assertEqual(code, 1)
        self.assertEqual(summary['abiV13']['action'], 'failed')
        self.assertIn('abi-v13', failure_stages(summary))
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())

    def test_v14_provider_uses_its_own_pinned_profile_preparation(self):
        code, summary, scripted, build_dir = self.run_profile(**{'v14-cat-file': 1})
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV14']['action'], 'prepared')
        fetches = [argv for argv in scripted.calls if argv[0] == 'git' and 'fetch' in argv]
        self.assertEqual(fetches, [['git', '-C', str(ROOT), 'fetch', '--no-tags', '--depth=1',
                                    'origin', V14_COMMIT]])
        self.assertIn('abi-v14-fetch', stage_names(summary))
        prepare = next(argv for argv in scripted.calls if V14_COMMIT in argv and '--tree' in argv)
        self.assertEqual(prepare[prepare.index('--tree') + 1], V14_TREE)
        self.assertEqual(Path(prepare[prepare.index('--output') + 1]).resolve(),
                         (build_dir / 'native-abi-v14').resolve())
        self.assertEqual(prepare[prepare.index('--header-manifest') + 1],
                         str(ROOT / 'tests/fixtures/native_cpp_abi/host-f736676/manifest.json'))
        self.assertEqual(Path(prepare[prepare.index('--current-build') + 1]).resolve(),
                         build_dir.resolve())
        self.assertEqual(prepare[prepare.index('--jobs') + 1], '2')

    def test_present_v14_object_does_not_fetch(self):
        code, summary, scripted, _ = self.run_profile(**{'v14-cat-file': 0})
        self.assertEqual(code, 0, summary['failures'])
        self.assertNotIn('v14-fetch', scripted.names())
        self.assertIn('v14-prepare', scripted.names())

    def test_v15_frozen_provider_uses_its_own_pinned_profile_preparation(self):
        code, summary, scripted, build_dir = self.run_profile(**{'v15-frozen-cat-file': 1})
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV15Frozen']['action'], 'prepared')
        fetches = [argv for argv in scripted.calls if argv[0] == 'git' and 'fetch' in argv]
        self.assertEqual(fetches, [['git', '-C', str(ROOT), 'fetch', '--no-tags', '--depth=1',
                                    'origin', V15_FROZEN_COMMIT]])
        self.assertIn('abi-v15-frozen-fetch', stage_names(summary))
        prepare = next(argv for argv in scripted.calls if V15_FROZEN_COMMIT in argv and '--tree' in argv)
        self.assertEqual(prepare[prepare.index('--tree') + 1], V15_FROZEN_TREE)
        self.assertEqual(Path(prepare[prepare.index('--output') + 1]).resolve(),
                         (build_dir / 'native-abi-v15-frozen').resolve())
        self.assertEqual(prepare[prepare.index('--header-manifest') + 1],
                         str(ROOT / 'tests/fixtures/native_cpp_abi/host-e7cdf05/manifest.json'))

    def test_v16_frozen_provider_uses_its_own_pinned_profile_preparation(self):
        code, summary, scripted, build_dir = self.run_profile(**{'v16-frozen-cat-file': 1})
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV16Frozen']['action'], 'prepared')
        fetches = [argv for argv in scripted.calls if argv[0] == 'git' and 'fetch' in argv]
        self.assertEqual(fetches, [['git', '-C', str(ROOT), 'fetch', '--no-tags', '--depth=1',
                                    'origin', V16_FROZEN_COMMIT]])
        self.assertIn('abi-v16-frozen-fetch', stage_names(summary))
        prepare = next(argv for argv in scripted.calls if V16_FROZEN_COMMIT in argv and '--tree' in argv)
        self.assertEqual(prepare[prepare.index('--tree') + 1], V16_FROZEN_TREE)
        self.assertEqual(Path(prepare[prepare.index('--output') + 1]).resolve(),
                         (build_dir / 'native-abi-v16-frozen').resolve())
        self.assertEqual(prepare[prepare.index('--header-manifest') + 1],
                         str(ROOT / 'tests/fixtures/native_cpp_abi/host-ab9714b/manifest.json'))

    def test_v18_frozen_provider_uses_its_own_pinned_profile_preparation(self):
        code, summary, scripted, build_dir = self.run_profile(**{'v18-frozen-cat-file': 1})
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV18Frozen']['action'], 'prepared')
        fetches = [argv for argv in scripted.calls if argv[0] == 'git' and 'fetch' in argv]
        self.assertEqual(fetches, [['git', '-C', str(ROOT), 'fetch', '--no-tags', '--depth=1',
                                    'origin', V18_FROZEN_COMMIT]])
        self.assertIn('abi-v18-frozen-fetch', stage_names(summary))
        prepare = next(argv for argv in scripted.calls if V18_FROZEN_COMMIT in argv and '--tree' in argv)
        self.assertEqual(prepare[prepare.index('--tree') + 1], V18_FROZEN_TREE)
        self.assertEqual(Path(prepare[prepare.index('--output') + 1]).resolve(),
                         (build_dir / 'native-abi-v18-frozen').resolve())
        self.assertEqual(prepare[prepare.index('--header-manifest') + 1],
                         str(ROOT / 'tests/fixtures/native_cpp_abi/host-fc7aad6/manifest.json'))

    def test_matching_v18_frozen_is_reused_without_fetch_or_prepare(self):
        code, summary, scripted, _ = self.run_profile(preexisting_v18_frozen='match')
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV18Frozen']['action'], 'reused')
        self.assertNotIn('v18-frozen-fetch', scripted.names())
        self.assertNotIn('v18-frozen-prepare', scripted.names())

    def test_matching_v15_frozen_is_reused_without_fetch_or_prepare(self):
        code, summary, scripted, _ = self.run_profile(preexisting_v15_frozen='match')
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV15Frozen']['action'], 'reused')
        self.assertNotIn('v15-frozen-fetch', scripted.names())
        self.assertNotIn('v15-frozen-prepare', scripted.names())

    def test_matching_v16_frozen_is_reused_without_fetch_or_prepare(self):
        code, summary, scripted, _ = self.run_profile(preexisting_v16_frozen='match')
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV16Frozen']['action'], 'reused')
        self.assertNotIn('v16-frozen-fetch', scripted.names())
        self.assertNotIn('v16-frozen-prepare', scripted.names())

    def test_matching_v14_is_reused_without_fetch_or_prepare(self):
        code, summary, scripted, _ = self.run_profile(preexisting_v14='match')
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['abiV14']['action'], 'reused')
        self.assertNotIn('v14-fetch', scripted.names())
        self.assertNotIn('v14-prepare', scripted.names())

    def test_mismatch_v14_refuses_without_deletion_and_keeps_other_checks(self):
        code, summary, scripted, build_dir = self.run_profile(preexisting_v14='mismatch')
        self.assertEqual(code, 1)
        self.assertEqual(summary['abiV14']['action'], 'refused')
        self.assertIn('abi-v14', failure_stages(summary))
        self.assertIn('fresh --build-dir', summary['failures'][0]['error'])
        self.assertTrue((build_dir / 'native-abi-v14/sentinel').is_file())
        self.assertTrue((build_dir / 'native-abi-v14/receipt.json').is_file())
        self.assertNotIn('v14-prepare', scripted.names())
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())

    def test_v14_directory_without_receipt_is_preserved_and_refused(self):
        code, summary, scripted, build_dir = self.run_profile(preexisting_v14='no-receipt')
        self.assertEqual(code, 1)
        self.assertEqual(summary['abiV14']['action'], 'refused')
        self.assertIn('abi-v14', failure_stages(summary))
        self.assertIn('without a receipt', summary['failures'][0]['error'])
        self.assertTrue((build_dir / 'native-abi-v14/sentinel').is_file())
        self.assertNotIn('v14-prepare', scripted.names())
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())

    def test_v14_provider_failure_is_reported_without_hiding_checks(self):
        code, summary, scripted, _ = self.run_profile(**{'v14-prepare': 1})
        self.assertEqual(code, 1)
        self.assertEqual(summary['abiV14']['action'], 'failed')
        self.assertIn('abi-v14', failure_stages(summary))
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())

    def test_v14_fetch_failure_does_not_prepare_and_keeps_other_checks(self):
        code, summary, scripted, _ = self.run_profile(**{'v14-cat-file': 1, 'v14-fetch': 1})
        self.assertEqual(code, 1)
        self.assertEqual(summary['abiV14']['action'], 'failed')
        self.assertIn('abi-v14-fetch', failure_stages(summary))
        self.assertNotIn('v14-prepare', scripted.names())
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())

    def test_present_prior_object_does_not_fetch(self):
        code, summary, scripted, _ = self.run_profile(**{'prior-cat-file': 0})
        self.assertEqual(code, 0, summary['failures'])
        self.assertNotIn('prior-fetch', scripted.names())
        self.assertIn('prior-prepare', scripted.names())

    def test_prior_fetch_failure_does_not_prepare_and_keeps_other_checks(self):
        code, summary, scripted, _ = self.run_profile(**{'prior-cat-file': 1, 'prior-fetch': 1})
        self.assertEqual(code, 1)
        self.assertEqual(summary['abiPrior']['action'], 'failed')
        self.assertIn('abi-prior-fetch', failure_stages(summary))
        self.assertNotIn('prior-prepare', scripted.names())
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())

    def test_require_websocket_rejects_skip(self):
        code, summary, scripted, _ = self.run_profile(
            'native', extra=['--require-websocket'], **{'require-websocket': 77})
        self.assertEqual(code, 1)
        self.assertIn('require-websocket', scripted.names())
        self.assertIn('require-websocket-skip', failure_stages(summary))
        self.assertTrue(any('cannot accept skip' in (item.get('error') or '')
                            for item in summary['failures']))

    def test_require_websocket_rejects_missing_binary(self):
        code, summary, scripted, _ = self.run_profile(
            'native', extra=['--require-websocket'], omit_ws_binary=True)
        self.assertEqual(code, 1)
        self.assertIn('require-websocket', failure_stages(summary))
        self.assertNotIn('require-websocket', scripted.names())

    def test_native_help_runs_after_install(self):
        code, summary, scripted, _ = self.run_profile('native')
        self.assertEqual(code, 0, summary['failures'])
        self.assertIn('native-help', stage_names(summary))
        names = stage_names(summary)
        self.assertLess(names.index('install'), names.index('native-help'))

    def test_sanitizer_missing_public_flag_fails_before_build(self):
        code, summary, scripted, _ = self.run_profile('sanitizers', sanitizer_flag='absent')
        self.assertEqual(code, 1)
        self.assertIn('sanitizer-public-flag', failure_stages(summary))
        self.assertNotIn('build', scripted.names())

    def test_sanitizers_profile_passes_public_flag_and_asan_env(self):
        code, summary, scripted, _ = self.run_profile('sanitizers')
        self.assertEqual(code, 0, summary['failures'])
        self.assertIn('sanitizer-public-flag', stage_names(summary))
        ctest = next(stage for stage in summary['stages'] if stage['name'] == 'ctest')
        self.assertEqual(ctest['extraEnvKeys'], ['ASAN_OPTIONS', 'UBSAN_OPTIONS'])
        self.assertNotIn('ctest', failure_stages(summary))

    def test_smoke_ignores_stderr_noise_and_checks_stdout(self):
        code, summary, _, _ = self.run_profile(**{'smoke-stderr': 'AddressSanitizer noise'})
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['actualVersion'], expected_version(ROOT))

    def test_logs_and_summary_omit_environment_secrets(self):
        secret = 's3cret-ci-verify-token-not-for-logs'
        previous = os.environ.get('CI_VERIFY_TEST_SECRET')
        os.environ['CI_VERIFY_TEST_SECRET'] = secret
        os.environ['AWS_SECRET_ACCESS_KEY'] = secret
        try:
            code, summary, scripted, build_dir = self.run_profile()
            self.assertEqual(code, 0, summary['failures'])
            dumped = json.dumps(summary)
            for path in (build_dir / 'ci-logs').glob('*'):
                dumped += path.read_text(errors='replace')
            self.assertNotIn(secret, dumped)
            self.assertFalse(scripted.secret_seen_in_argv)
            self.assertNotIn('AWS_SECRET_ACCESS_KEY', dumped)
        finally:
            os.environ.pop('AWS_SECRET_ACCESS_KEY', None)
            if previous is None:
                os.environ.pop('CI_VERIFY_TEST_SECRET', None)
            else:
                os.environ['CI_VERIFY_TEST_SECRET'] = previous

    def test_ctest_junit_detection_matches_real_ctest(self):
        help_text = default_runner(['ctest', '--help'], extra_env=None, timeout=30,
                                   combine_stderr=True, stream_output=False)
        self.assertEqual(help_text.returncode, 0)
        self.assertEqual(ctest_supports_junit(default_runner),
                         '--output-junit' in help_text.stdout.decode())


class RowFloorDriver(unittest.TestCase):
    run_profile = DriverOrderingAndAggregation.run_profile

    def test_kernel_floor_passes_when_ctest_reports_enough_rows(self):
        code, summary, _, _ = self.run_profile('kernel')
        self.assertEqual(code, 0, summary['failures'])
        names = stage_names(summary)
        self.assertIn('ctest-floor', names)
        self.assertLess(names.index('ctest'), names.index('ctest-floor'))
        self.assertLess(names.index('ctest-floor'), names.index('install'))
        self.assertEqual(summary['minTests'], KERNEL_MIN_TESTS)
        self.assertEqual(summary['ctestRows'], KERNEL_MIN_TESTS)

    def test_kernel_floor_fails_below_it_without_hiding_package_checks(self):
        code, summary, scripted, _ = self.run_profile('kernel', ctest_rows=KERNEL_MIN_TESTS - 1)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertNotIn('ctest', failure_stages(summary))
        self.assertIn('install', scripted.names())
        self.assertIn('smoke-version', scripted.names())
        error = next(item['error'] for item in summary['failures'] if item['stage'] == 'ctest-floor')
        self.assertIn(str(KERNEL_MIN_TESTS - 1), error)
        self.assertIn(str(KERNEL_MIN_TESTS), error)
        self.assertEqual(summary['ctestRows'], KERNEL_MIN_TESTS - 1)

    def test_kernel_floor_fails_closed_without_a_row_count(self):
        code, summary, _, _ = self.run_profile('kernel', ctest_rows='absent')
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertIsNone(summary['ctestRows'])

    def test_release_floor_fails_below_it_without_hiding_package_checks(self):
        code, summary, scripted, _ = self.run_profile('release', ctest_rows=RELEASE_MIN_TESTS - 1)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertNotIn('ctest', failure_stages(summary))
        self.assertIn('install', scripted.names())
        self.assertIn('smoke-version', scripted.names())
        error = next(item['error'] for item in summary['failures'] if item['stage'] == 'ctest-floor')
        self.assertIn(str(RELEASE_MIN_TESTS - 1), error)
        self.assertIn(str(RELEASE_MIN_TESTS), error)
        self.assertIn('release profile', error)
        self.assertEqual(summary['minTests'], RELEASE_MIN_TESTS)

    def test_release_floor_passes_at_it_and_fails_closed_without_a_row_count(self):
        code, summary, _, _ = self.run_profile('release')
        self.assertEqual(code, 0, summary['failures'])
        names = stage_names(summary)
        self.assertLess(names.index('ctest'), names.index('ctest-floor'))
        self.assertEqual(summary['ctestRows'], RELEASE_MIN_TESTS)
        code, summary, _, _ = self.run_profile('release', ctest_rows='absent')
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))

    def test_floor_counts_rows_that_ran_and_lists_a_skipped_row_beside_them(self):
        # The kernel profile as it runs where libcurl lacks WebSocket support:
        # one row more registered than the floor, and that row skipped.
        code, summary, _, build_dir = self.run_profile(
            'kernel', ctest_rows=KERNEL_MIN_TESTS + 1,
            ctest_skipped=['test_native_live_websocket'])
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['ctestRows'], KERNEL_MIN_TESTS)
        self.assertEqual(summary['ctestTotal'], KERNEL_MIN_TESTS + 1)
        self.assertEqual(summary['ctestSkipped'], ['test_native_live_websocket'])
        self.assertEqual(summary['ctestNotRun'], [])
        self.assertEqual(summary['ctestDisabled'], [])
        log = (build_dir / 'ci-logs' / 'ctest-floor.log').read_text()
        self.assertIn(f'ctest ran {KERNEL_MIN_TESTS} rows (floor {KERNEL_MIN_TESTS})', log)
        self.assertIn('not counted: 1 skipped (test_native_live_websocket)', log)

    def test_a_row_that_stops_running_trips_the_floor_though_ctest_still_counts_it(self):
        # CTest's own count includes a skipped row, so before lane Q12 a row
        # that began to skip left the count at the floor and passed.
        for profile, floor in (('kernel', KERNEL_MIN_TESTS), ('release', RELEASE_MIN_TESTS)):
            with self.subTest(profile=profile):
                code, summary, scripted, _ = self.run_profile(
                    profile, ctest_rows=floor, ctest_skipped=['test_quietly_skipping'])
                self.assertEqual(code, 1)
                self.assertIn('ctest-floor', failure_stages(summary))
                self.assertNotIn('ctest', failure_stages(summary))
                self.assertIn('install', scripted.names())
                self.assertEqual(summary['ctestTotal'], floor)
                self.assertEqual(summary['ctestRows'], floor - 1)
                error = next(item['error'] for item in summary['failures']
                             if item['stage'] == 'ctest-floor')
                self.assertIn(f'ctest ran {floor - 1} rows, below the floor of {floor}', error)
                self.assertIn('1 skipped (test_quietly_skipping)', error)

    def test_a_row_ctest_could_not_start_is_not_counted_either(self):
        code, summary, _, _ = self.run_profile(
            'kernel', ctest=8, ctest_rows=KERNEL_MIN_TESTS, ctest_not_run=['test_missing_binary'])
        self.assertEqual(code, 1)
        self.assertIn('ctest', failure_stages(summary))
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertEqual(summary['ctestRows'], KERNEL_MIN_TESTS - 1)
        self.assertEqual(summary['ctestNotRun'], ['test_missing_binary'])

    def test_the_floor_still_trips_on_a_real_collapse(self):
        # Every kernel row but one skips, with CTest's count still at the
        # floor; and a release suite that simply shrank.
        skipped = [f'test_row_{number}' for number in range(KERNEL_MIN_TESTS - 1)]
        code, summary, _, _ = self.run_profile(
            'kernel', ctest_rows=KERNEL_MIN_TESTS, ctest_skipped=skipped)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertEqual(summary['ctestRows'], 1)
        code, summary, _, _ = self.run_profile('release', ctest_rows=12)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertEqual(summary['ctestRows'], 12)

    def test_floor_fails_closed_on_row_lists_it_cannot_read(self):
        code, summary, _, _ = self.run_profile(
            'kernel', ctest=8, ctest_raw=b'98% tests passed, 3 tests failed out of 999\n')
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertIsNone(summary['ctestRows'])
        self.assertEqual(summary['ctestTotal'], 999)
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'ctest-floor')
        self.assertIn('cannot read', error)

    def test_min_tests_override_gates_the_rows_that_ran(self):
        # --min-tests stays the override (fail-closed below 1, see
        # RowFloorConfig) and gates the same count: 4 rows, 1 skipped.
        code, summary, _, _ = self.run_profile(
            'debug', extra=['--min-tests', '3'], ctest_rows=4, ctest_skipped=['test_skip'])
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['ctestRows'], 3)
        code, summary, _, _ = self.run_profile(
            'debug', extra=['--min-tests', '4'], ctest_rows=4, ctest_skipped=['test_skip'])
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))

    def test_profiles_without_a_floor_record_rows_but_gate_nothing(self):
        # expectation corrected: a 3-row release run passed ungated -> release
        # left this loop, because lane P7 gives it a row floor (the two tests
        # above pin that floor).
        for profile in ('debug', 'sanitizers', 'native'):
            with self.subTest(profile=profile):
                code, summary, _, _ = self.run_profile(profile, ctest_rows=3)
                self.assertEqual(code, 0, summary['failures'])
                self.assertNotIn('ctest-floor', stage_names(summary))
                self.assertIsNone(summary['minTests'])
                self.assertEqual(summary['ctestRows'], 3)

    def test_min_tests_gates_any_profile_and_overrides_the_kernel_default(self):
        for profile in ('release', 'debug'):
            with self.subTest(profile=profile):
                code, summary, _, _ = self.run_profile(
                    profile, extra=['--min-tests', '4'], ctest_rows=3)
                self.assertEqual(code, 1)
                self.assertIn('ctest-floor', failure_stages(summary))
                self.assertEqual(summary['minTests'], 4)
        for profile in ('kernel', 'release'):
            with self.subTest(profile=profile):
                code, summary, _, _ = self.run_profile(
                    profile, extra=['--min-tests', '2'], ctest_rows=3)
                self.assertEqual(code, 0, summary['failures'])
                self.assertEqual(summary['minTests'], 2)
                self.assertIn('ctest-floor', stage_names(summary))


class ExamplesAssertLive(unittest.TestCase):
    run_profile = DriverOrderingAndAggregation.run_profile

    def test_ndebug_follows_the_compiler_reading_order(self):
        for argv, defined in (
                ([], False),
                (['-O3', '-DNDEBUG'], True),
                (['-O3', '-DNDEBUG', '-ffp-contract=off', '-UNDEBUG'], False),
                (['-UNDEBUG', '-DNDEBUG'], True),
                (['-D', 'NDEBUG'], True),
                (['-D', 'NDEBUG', '-U', 'NDEBUG'], False),
                (['-DNDEBUG=1'], True),
                (['-DNDEBUG_EXTRA', '-UNDEBUGGING'], False)):
            with self.subTest(argv=argv):
                self.assertEqual(ci_verify.ndebug_defined(argv), defined)

    # The profiles that compile an examples/native source -- the examples
    # (release, kernel) or the live runner's two MODULE builds of example
    # sources (kernel, native) -- and the targets the scripted configure
    # compiles from them.
    ASSERT_LIVE_TARGETS = {
        'release': ['example_hello_kernel', 'example_hello_kernel_c'],
        'kernel': ['example_hello_kernel', 'example_hello_kernel_c',
                   'native_market_example', 'native_selected_example'],
        'native': ['native_market_example', 'native_selected_example'],
    }

    def test_every_example_compiled_with_assert_live_passes_before_the_build(self):
        # expectation corrected: the stage ran in release and kernel and read
        # the example_* targets alone ('all 2 example targets: ...'; the
        # runner's MODULE builds of two example sources kept NDEBUG and were
        # not counted) -> it runs in every profile that compiles an
        # examples/native source, native included, and counts those modules,
        # because runner/CMakeLists.txt now gives them -UNDEBUG too (lane E4,
        # item 2).
        for profile, targets in self.ASSERT_LIVE_TARGETS.items():
            with self.subTest(profile=profile):
                code, summary, _, build_dir = self.run_profile(profile)
                self.assertEqual(code, 0, summary['failures'])
                names = stage_names(summary)
                self.assertLess(names.index('profile-options'),
                                names.index('examples-assert-live'))
                self.assertLess(names.index('examples-assert-live'), names.index('build'))
                log = (build_dir / 'ci-logs' / 'examples-assert-live.log').read_text()
                self.assertIn(f'all {len(targets)} targets built from examples/native sources: '
                              + ', '.join(targets), log)

    def test_an_example_compiled_with_ndebug_fails_before_the_build(self):
        # The mutation the stage exists for: one compile of an example source
        # without -UNDEBUG after Release's -DNDEBUG -- an example_* target, or
        # a live-runner module built from one (lane E4, item 2).
        for profile, target in (('release', 'example_hello_kernel_c'),
                                ('kernel', 'example_hello_kernel_c'),
                                ('kernel', 'native_market_example'),
                                ('native', 'native_selected_example')):
            with self.subTest(profile=profile, target=target):
                code, summary, scripted, _ = self.run_profile(profile, example_ndebug=target)
                self.assertEqual(code, 1)
                self.assertEqual(failure_stages(summary), ['examples-assert-live'])
                self.assertNotIn('build', scripted.names())
                self.assertIn(f'NDEBUG stays defined in the compile of {target}, so',
                              summary['failures'][0]['error'])

    def test_the_stage_fails_closed_without_example_compile_commands(self):
        # expectation corrected: 'no example_* compile command' -> 'no compile
        # of an examples/native source', because the stage reads every compile
        # of an example source, the runner's modules included, and fails
        # closed only when there is none (lane E4, item 2).
        for profile in ('kernel', 'native'):
            for mode, needle in (('absent', 'compile_commands.json missing'),
                                 ('none', 'no compile of an examples/native source')):
                with self.subTest(profile=profile, mode=mode):
                    code, summary, scripted, _ = self.run_profile(
                        profile, example_commands=mode)
                    self.assertEqual(code, 1)
                    self.assertEqual(failure_stages(summary), ['examples-assert-live'])
                    self.assertIn(needle, summary['failures'][0]['error'])
                    self.assertNotIn('build', scripted.names())

    def test_profiles_that_compile_no_example_source_do_not_run_the_stage(self):
        # expectation corrected: ('debug', 'sanitizers', 'native') ->
        # ('debug', 'sanitizers'), because the native profile builds the live
        # runner's two modules from examples/native sources and now runs the
        # stage (lane E4, item 2).
        for profile in ('debug', 'sanitizers'):
            with self.subTest(profile=profile):
                code, summary, _, _ = self.run_profile(profile)
                self.assertEqual(code, 0, summary['failures'])
                self.assertNotIn('examples-assert-live', stage_names(summary))


class StaleBinaries(unittest.TestCase):
    """stale-binaries holds each archive against the files its own compiles read.

    Until lane E4 it held libpineforge.a against every file under src/,
    include/ and CMakeLists.txt, so the kernel profile, whose archives never
    compile src/source/ or src/compat/pine/, failed after any edit there
    (lane P7's report), and every profile failed after an edit to a header
    only examples or tests include. A synthetic tree pins the reading; the
    driver rows pin the wiring.
    """
    run_profile = DriverOrderingAndAggregation.run_profile
    FILES = {
        'CMakeLists.txt': 'project(fixture)\n',
        'include/pineforge/kernel.hpp':
            '#pragma once\n#include "pineforge/detail.hpp"\n#include <vector>\n'
            '#include <pineforge/version.h>\n',
        'include/pineforge/detail.hpp': '#pragma once\n',
        'include/pineforge/module.hpp': '#pragma once\n#include "pineforge/kernel.hpp"\n',
        'include/pineforge/source/adapter.hpp': '#pragma once\n#include "pineforge/kernel.hpp"\n',
        'src/kernel.cpp': '#include "pineforge/kernel.hpp"\n#include "local.hpp"\n',
        'src/local.hpp': '#pragma once\n',
        'src/source/adapter.cpp': '#include "pineforge/source/adapter.hpp"\n',
    }
    KERNEL_UNITS = ('src/kernel.cpp',)
    ALL_UNITS = ('src/kernel.cpp', 'src/source/adapter.cpp')

    def fixture(self, units_of: dict) -> tuple[Path, Path]:
        """A source tree and a build tree whose compile database compiles
        units_of[target] into lib/lib<target>.a, each archive built after
        every source file."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve()
        source, build = root / 'source', root / 'build'
        for rel, text in self.FILES.items():
            path = source / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            os.utime(path, (1000, 1000))
        entries = []
        for target, units in units_of.items():
            for unit in units:
                obj = f'CMakeFiles/{target}.dir/{unit}.o'
                entries.append({
                    'directory': str(build), 'file': str(source / unit), 'output': obj,
                    'command': f'c++ -I{source}/include -I{build}/include -I{source}/src '
                               f'-isystem /usr/include -O2 -o {obj} -c {source / unit}'})
            archive = build / 'lib' / f'lib{target}.a'
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(b'!<arch>\n')
            os.utime(archive, (2000, 2000))
        (build / 'compile_commands.json').write_text(json.dumps(entries))
        return source, build

    @staticmethod
    def edit(source: Path, *files: str) -> None:
        for rel in files:
            os.utime(source / rel, (3000, 3000))

    @staticmethod
    def stale(source: Path, build: Path, target: str) -> list[str]:
        return ci_verify.stale_archive_sources(source, build / 'lib' / f'lib{target}.a')

    def test_a_kernel_only_archive_ignores_what_it_never_compiles(self):
        # The kernel profile, where libpineforge.a is the kernel alone.
        source, build = self.fixture({'pineforge': self.KERNEL_UNITS})
        self.edit(source, 'src/source/adapter.cpp', 'include/pineforge/source/adapter.hpp',
                  'include/pineforge/module.hpp', 'CMakeLists.txt')
        self.assertEqual(self.stale(source, build, 'pineforge'), [])

    def test_every_file_the_archive_compiles_is_held_against_it(self):
        # A unit, a header reached through another header, and a "quoted"
        # header found beside the file that includes it.
        for edited in ('src/kernel.cpp', 'include/pineforge/detail.hpp', 'src/local.hpp'):
            with self.subTest(edited=edited):
                source, build = self.fixture({'pineforge': self.KERNEL_UNITS})
                self.edit(source, edited)
                self.assertEqual(self.stale(source, build, 'pineforge'), [edited])

    def test_each_archive_is_held_against_its_own_compiles(self):
        # The default build: libpineforge.a compiles the source layer too,
        # libpineforge_kernel.a never does.
        source, build = self.fixture({'pineforge': self.ALL_UNITS,
                                      'pineforge_kernel': self.KERNEL_UNITS})
        self.edit(source, 'include/pineforge/source/adapter.hpp', 'include/pineforge/module.hpp')
        self.assertEqual(self.stale(source, build, 'pineforge'),
                         ['include/pineforge/source/adapter.hpp'])
        self.assertEqual(self.stale(source, build, 'pineforge_kernel'), [])

    def test_a_header_generated_into_the_build_tree_is_an_input(self):
        # CMake writes pineforge/version.h into the build tree; kernel.hpp
        # reaches it through -I<build>/include, wherever that tree lives.
        # Outside both trees (the standard library) nothing is followed.
        source, build = self.fixture({'pineforge': self.KERNEL_UNITS})
        generated = build / 'include' / 'pineforge' / 'version.h'
        generated.parent.mkdir(parents=True)
        generated.write_text('#define PINEFORGE_GIT_SHA "0000000"\n')
        os.utime(generated, (1000, 1000))
        self.assertIn(generated, ci_verify.archive_inputs(source, build, 'pineforge'))
        self.assertEqual(self.stale(source, build, 'pineforge'), [])
        os.utime(generated, (3000, 3000))
        self.assertEqual(self.stale(source, build, 'pineforge'), [str(generated)])

    def test_the_inputs_are_the_units_and_the_headers_they_reach_in_the_tree(self):
        source, build = self.fixture({'pineforge': self.ALL_UNITS})
        inputs = ci_verify.archive_inputs(source, build, 'pineforge')
        self.assertEqual([str(path.relative_to(source)) for path in inputs], [
            'include/pineforge/detail.hpp', 'include/pineforge/kernel.hpp',
            'include/pineforge/source/adapter.hpp', 'src/kernel.cpp', 'src/local.hpp',
            'src/source/adapter.cpp'])

    def test_it_fails_closed_when_it_cannot_list_an_archive_inputs(self):
        source, build = self.fixture({'pineforge_kernel': self.KERNEL_UNITS})
        (build / 'lib' / 'libpineforge.a').write_bytes(b'!<arch>\n')
        with self.assertRaisesRegex(RuntimeError, 'no compile command of target pineforge$'):
            self.stale(source, build, 'pineforge')
        (build / 'compile_commands.json').unlink()
        with self.assertRaisesRegex(RuntimeError, 'compile_commands.json missing'):
            self.stale(source, build, 'pineforge_kernel')
        with self.assertRaisesRegex(RuntimeError, 'did not produce'):
            self.stale(source, build, 'pineforge_live_support')

    def test_every_profile_holds_both_archives_after_the_build(self):
        for profile in PROFILES:
            with self.subTest(profile=profile):
                code, summary, _, build_dir = self.run_profile(profile)
                self.assertEqual(code, 0, summary['failures'])
                names = stage_names(summary)
                self.assertLess(names.index('build'), names.index('stale-binaries'))
                self.assertIn('libpineforge.a and libpineforge_kernel.a are newer than every '
                              'file', (build_dir / 'ci-logs' / 'stale-binaries.log').read_text())

    def test_a_stale_archive_fails_the_stage_before_ctest(self):
        for name in ('libpineforge.a', 'libpineforge_kernel.a'):
            with self.subTest(archive=name):
                code, summary, scripted, _ = self.run_profile('kernel', stale_archive=name)
                self.assertEqual(code, 1)
                self.assertEqual(failure_stages(summary), ['stale-binaries'])
                error = summary['failures'][0]['error']
                self.assertTrue(error.startswith(f'{name} predates '), error)
                self.assertIn('src/matrix.cpp', error)
                self.assertNotIn('ctest', scripted.names())

    def test_a_build_without_the_archive_compiles_fails_closed(self):
        code, summary, scripted, _ = self.run_profile('release', library_commands='none')
        self.assertEqual(code, 1)
        self.assertEqual(failure_stages(summary), ['stale-binaries'])
        self.assertIn('no compile command of target pineforge', summary['failures'][0]['error'])
        self.assertNotIn('ctest', scripted.names())


class ReceiptRecipe(unittest.TestCase):
    """check_abi_receipt_skips.py --prepare, the recipe for a plain build tree
    (CLAUDE.md, docs/ci.md), prepares the six providers exactly as ci_verify
    does, so a tree prepared by hand is one ci_verify would reuse."""
    run_profile = DriverOrderingAndAggregation.run_profile

    def test_the_recipe_prepares_every_provider_with_ci_verify_argv(self):
        code, summary, scripted, build_dir = self.run_profile('release')
        self.assertEqual(code, 0, summary['failures'])
        prepared = [argv for argv in scripted.calls
                    if any(Path(part).name == 'prepare_settlement_cpp_abi_base.py'
                           for part in argv)]
        self.assertEqual(prepared, [
            check_abi_receipt_skips.prepare_argv(ROOT, build_dir.resolve(), role, 2)
            for role in check_abi_receipt_skips.PROVIDER_ROLES])

class DiagnosticsCollection(unittest.TestCase):
    def collect(self, build: Path, output: Path) -> None:
        env = os.environ.copy()
        env.pop('GITHUB_STEP_SUMMARY', None)
        result = subprocess.run(
            [sys.executable, str(ROOT / 'scripts/collect_ci_diagnostics.py'),
             '--build-dir', str(build), '--profile', 'release', '--output', str(output)],
            capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_v13_provider_diagnostics_survive_without_binaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            provider = root / 'build/native-abi-v13'
            provider.mkdir(parents=True)
            files = {
                'receipt.json': json.dumps({'commit': V13_COMMIT, 'archiveSha256': 'a' * 64}),
                'configure.log': 'v13 provider configured\n',
                'build.log': 'v13 provider built\n',
            }
            for name, content in files.items():
                (provider / name).write_text(content)
            (provider / 'libpineforge.a').write_bytes(b'!<arch>\nexcluded library\n')
            (provider / 'source.tar').write_bytes(b'excluded source archive\n')
            output = root / 'diagnostics'
            self.collect(root / 'build', output)
            for name, content in files.items():
                self.assertEqual((output / ('native-abi-v13-' + name)).read_text(), content)
            missing = json.loads((output / 'missing.json').read_text())
            self.assertFalse(any(name.startswith('native-abi-v13/') for name in missing))
            retained = [path.name for path in output.rglob('*') if path.is_file()]
            self.assertNotIn('libpineforge.a', retained)
            self.assertNotIn('source.tar', retained)

    def test_failed_v13_preparation_retains_partial_logs_and_records_missing_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            provider = root / 'build/native-abi-v13'
            provider.mkdir(parents=True)
            (provider / 'configure.log').write_text('v13 configure failure details\n')
            output = root / 'diagnostics'
            self.collect(root / 'build', output)
            self.assertEqual((output / 'native-abi-v13-configure.log').read_text(),
                             'v13 configure failure details\n')
            missing = json.loads((output / 'missing.json').read_text())
            self.assertIn('native-abi-v13/receipt.json', missing)
            self.assertIn('native-abi-v13/build.log', missing)
            self.assertNotIn('native-abi-v13/configure.log', missing)

    def test_v14_provider_diagnostics_survive_without_binaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            provider = root / 'build/native-abi-v14'
            provider.mkdir(parents=True)
            files = {
                'receipt.json': json.dumps({'commit': V14_COMMIT, 'archiveSha256': 'a' * 64}),
                'configure.log': 'v14 provider configured\n',
                'build.log': 'v14 provider built\n',
            }
            for name, content in files.items():
                (provider / name).write_text(content)
            (provider / 'libpineforge.a').write_bytes(b'!<arch>\nexcluded library\n')
            (provider / 'source.tar').write_bytes(b'excluded source archive\n')
            output = root / 'diagnostics'
            self.collect(root / 'build', output)
            for name, content in files.items():
                self.assertEqual((output / ('native-abi-v14-' + name)).read_text(), content)
            missing = json.loads((output / 'missing.json').read_text())
            self.assertFalse(any(name.startswith('native-abi-v14/') for name in missing))
            retained = [path.name for path in output.rglob('*') if path.is_file()]
            self.assertNotIn('libpineforge.a', retained)
            self.assertNotIn('source.tar', retained)

    def test_failed_v14_preparation_retains_partial_logs_and_records_missing_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            provider = root / 'build/native-abi-v14'
            provider.mkdir(parents=True)
            (provider / 'configure.log').write_text('v14 configure failure details\n')
            output = root / 'diagnostics'
            self.collect(root / 'build', output)
            self.assertEqual((output / 'native-abi-v14-configure.log').read_text(),
                             'v14 configure failure details\n')
            missing = json.loads((output / 'missing.json').read_text())
            self.assertIn('native-abi-v14/receipt.json', missing)
            self.assertIn('native-abi-v14/build.log', missing)
            self.assertNotIn('native-abi-v14/configure.log', missing)

    def test_v15_frozen_provider_diagnostics_survive_without_binaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            provider = root / 'build/native-abi-v15-frozen'
            provider.mkdir(parents=True)
            files = {
                'receipt.json': json.dumps({'commit': V15_FROZEN_COMMIT, 'archiveSha256': 'a' * 64}),
                'configure.log': 'frozen v15 provider configured\n',
                'build.log': 'frozen v15 provider built\n',
            }
            for name, content in files.items():
                (provider / name).write_text(content)
            (provider / 'libpineforge.a').write_bytes(b'!<arch>\nexcluded library\n')
            (provider / 'source.tar').write_bytes(b'excluded source archive\n')
            output = root / 'diagnostics'
            self.collect(root / 'build', output)
            for name, content in files.items():
                self.assertEqual((output / ('native-abi-v15-frozen-' + name)).read_text(), content)
            missing = json.loads((output / 'missing.json').read_text())
            self.assertFalse(any(name.startswith('native-abi-v15-frozen/') for name in missing))
            retained = [path.name for path in output.rglob('*') if path.is_file()]
            self.assertNotIn('libpineforge.a', retained)
            self.assertNotIn('source.tar', retained)

    def test_native_abi_control_receipt_is_retained_or_recorded_missing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build'
            build.mkdir()
            output = root / 'missing-diagnostics'
            self.collect(build, output)
            self.assertIn('native-abi-receipt.json', json.loads((output / 'missing.json').read_text()))
            receipt = {'status': 'passed', 'executedBinaries': 0,
                       'control': 'v14_current_execution_shape_agnostic_compile'}
            (build / 'native-abi-receipt.json').write_text(json.dumps(receipt))
            output = root / 'complete-diagnostics'
            self.collect(build, output)
            self.assertEqual(json.loads((output / 'native-abi-receipt.json').read_text()), receipt)
            self.assertNotIn('native-abi-receipt.json',
                             json.loads((output / 'missing.json').read_text()))


if __name__ == '__main__':
    unittest.main()
