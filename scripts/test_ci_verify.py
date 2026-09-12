#!/usr/bin/env python3
"""Driver control-flow, config, aggregation, and receipt-reuse tests for ci_verify.

Source guards, tool help, git object presence, compiler identity, and VERSION
are real. Full engine configure/build/CTest is not substituted as a passing
root verification — those stages are scripted here so failure aggregation and
ordering can be asserted. Root must still run the actual profiles.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

import ci_verify
from ci_verify import (
    Completed,
    ConfigError,
    ROOT,
    SANITIZER_FLAG,
    SANITIZER_RUN_ENV,
    cmake_cache_definitions,
    cmake_configure_argv,
    ctest_supports_junit,
    default_build_dir,
    default_runner,
    expected_version,
    main,
    parse_args,
    source_guard_commands,
    validate_config,
)
from prepare_settlement_cpp_abi_base import (
    BASE_COMMIT,
    BASE_TREE,
    COPY_CACHE,
    compiler_identity,
    copied_cache,
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
        if argv[0] == 'git' and 'cat-file' in argv:
            if 'cat-file' in self.exits:
                return Completed(int(self.exits['cat-file']), b'', b'')
            return default_runner(argv, extra_env=None, timeout=timeout,
                                  combine_stderr=True, stream_output=False)
        if argv[0] == 'git' and 'fetch' in argv:
            return Completed(int(self.exits.get('fetch', 0)), b'fetched\n', b'')
        if any(Path(part).name == 'prepare_settlement_cpp_abi_base.py' for part in argv):
            return Completed(int(self.exits.get('prepare', 0)), b'prepared\n', b'')
        if any(Path(part).suffix == '.py' for part in argv):
            needles = {
                'source-guard-c-abi': 'check_c_abi_runtime.py',
                'source-guard-broker-hash': 'check_broker_state_hash_coverage.py',
                'source-guard-pending-mirror': 'gen_pending_order_mirror.py',
                'source-guard-native-versions': 'check_native_cpp_versions.py',
                'source-guard-aggregate-versions': 'check_aggregate_cpp_versions.py',
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
            env_ok = True
            if self.profile == 'sanitizers':
                env_ok = extra_env == SANITIZER_RUN_ENV
            if not env_ok:
                return Completed(1, b'', b'sanitizer env missing\n')
            return Completed(int(self.exits.get('ctest', 0)), b'tests\n', b'')
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
        tutorial = 'OFF' if self.profile == 'native' else 'ON'
        live = 'ON' if self.profile == 'native' else 'OFF'
        sanitizers = 'ON' if self.profile == 'sanitizers' else 'OFF'
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
            'PINEFORGE_ENABLE_SANITIZERS': sanitizers,
            'PINEFORGE_VERSION_SOURCE': 'FILE',
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
        if self.profile == 'sanitizers':
            commands = [{
                'directory': str(self.build_dir),
                'file': str(self.source / 'src/matrix.cpp'),
                'command': f'{self.cxx} {SANITIZER_FLAG} -c src/matrix.cpp',
            }]
            if self.exits.get('sanitizer_flag') == 'absent':
                commands[0]['command'] = f'{self.cxx} -c src/matrix.cpp'
            (self.build_dir / 'compile_commands.json').write_text(json.dumps(commands))
        self._maybe_seed_abi_base()
        return Completed(0, b'configured\n', b'')

    def _build(self) -> Completed:
        code = int(self.exits.get('build', 0))
        if code != 0:
            return Completed(code, b'', b'build failed\n')
        archive = self.build_dir / 'lib' / 'libpineforge.a'
        archive.parent.mkdir(parents=True, exist_ok=True)
        archive.write_bytes(b'!<arch>\nci-verify-test\n')
        if self.profile == 'native' or self.exits.get('create_native_binaries'):
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
        if self.profile == 'native':
            help_bin = prefix / 'bin' / 'pineforge-live'
            help_bin.parent.mkdir(parents=True, exist_ok=True)
            help_bin.write_bytes(b'live')
        (self.build_dir / 'ci-smoke').mkdir(parents=True, exist_ok=True)
        (self.build_dir / 'ci-smoke' / 'smoke_version').write_bytes(b'smoke')
        return Completed(0, b'installed\n', b'')

    def _maybe_seed_abi_base(self) -> None:
        kind = self.exits.get('preexisting_base')
        if not kind:
            return
        output = self.build_dir / 'settlement-abi-base'
        output.mkdir(parents=True, exist_ok=True)
        (output / 'sentinel').write_text('keep\n')
        if kind == 'no-receipt':
            return
        archive = output / 'build' / 'lib' / 'libpineforge.a'
        headers = output / 'r2-headers.tar'
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
            'commit': BASE_COMMIT,
            'tree': BASE_TREE,
            'archive': 'build/lib/libpineforge.a',
            'archiveSha256': identity(archive)['sha256'],
            'headers': 'r2-headers.tar',
            'headersSha256': identity(headers)['sha256'],
            'generatedInclude': 'build/include',
            'generatedHeaderSha256': identity(generated)['sha256'],
            'compiler': compiler,
            'copiedCurrentCache': cache,
        }
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')

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
                names.append('fetch')
            elif any(Path(part).name == 'prepare_settlement_cpp_abi_base.py' for part in argv):
                names.append('prepare')
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
        for name in ('release', 'debug', 'sanitizers', 'native'):
            self.assertEqual(default_build_dir(ROOT, name), ROOT / f'build-ci-{name}')
            args = parse_args([name], source=ROOT)
            self.assertEqual(Path(args.build_dir), ROOT / f'build-ci-{name}')

    def test_build_dir_cannot_be_source_root(self):
        with self.assertRaisesRegex(ConfigError, 'source root'):
            validate_config(parse_args(['release', '--build-dir', str(ROOT)]))


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

    def seed(self, *, version='FILE', corrupt=False, receipt=True):
        archive = self.output / 'build' / 'lib' / 'libpineforge.a'
        headers = self.output / 'r2-headers.tar'
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
            'commit': BASE_COMMIT,
            'tree': BASE_TREE,
            'archive': 'build/lib/libpineforge.a',
            'archiveSha256': identity(archive)['sha256'],
            'headers': 'r2-headers.tar',
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


def read_cache_for_test(path: Path) -> dict[str, str]:
    from prepare_settlement_cpp_abi_base import read_cache
    return read_cache(path)


class DriverOrderingAndAggregation(unittest.TestCase):
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
        self.assertIn('ctest', scripted.names())
        self.assertIn('install', scripted.names())
        self.assertTrue((build_dir / 'ci-logs' / 'ctest.log').is_file())
        self.assertTrue((build_dir / 'ci-summary.json').is_file())
        ctest_argv = next(argv for argv in scripted.calls if argv[0] == 'ctest' and '--test-dir' in argv)
        if ctest_supports_junit(scripted):
            self.assertIn('--output-junit', ctest_argv)
            self.assertEqual(Path(ctest_argv[ctest_argv.index('--output-junit') + 1]).resolve(),
                             (build_dir / 'ctest-junit.xml').resolve())

    def test_junit_flag_omitted_when_unsupported(self):
        code, summary, scripted, build_dir = self.run_profile(junit_help='absent')
        self.assertEqual(code, 0, summary['failures'])
        ctest_argv = next(argv for argv in scripted.calls if argv[0] == 'ctest' and '--test-dir' in argv)
        self.assertNotIn('--output-junit', ctest_argv)

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


if __name__ == '__main__':
    unittest.main()
