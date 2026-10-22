#!/usr/bin/env python3
"""Driver control-flow, config, aggregation, and receipt-reuse tests for ci_verify.

Source guards, tool help, compiler identity, and VERSION are real. The scripted
driver has an explicit Git object inventory; a separate tiny Git fixture checks
real object discovery. Full engine configure/build/CTest is not substituted as a passing
root verification — those stages are scripted here so failure aggregation and
ordering can be asserted. Root must still run the actual profiles.
"""
from __future__ import annotations

import ast
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
    LIVE_SANITIZERS_MIN_TESTS,
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


_SENTINEL = object()
_PRIOR_OVERLAY_FLAG: object = _SENTINEL

# The campaign's per-profile floors, read from the one table in ci_verify so a
# test never restates a number the table owns. `FULL` is the overlay-present
# floor (every one of those rows must run); `ABSENT` is the public floor, the
# full population minus the pinned private-fixture rows that profile registers
# as SKIP placeholders without the overlay.
FULL_FLOOR = {name: entry[0] for name, entry in ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS.items()}
ABSENT_FLOOR = {name: entry[0] - entry[1]
                for name, entry in ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS.items()}


def guard_environment() -> dict:
    """The environment the harness hands a REAL source guard (`extra_env`).

    The ctest row pins TMPDIR to ``<build>/tmp`` (tests/CMakeLists.txt,
    ENVIRONMENT), and a ctest build dir is a child of the source tree, so the
    pin resolves inside the repository. check_native_cpp_versions.py refuses
    that -- 'system temporary directory is inside the repository', raised
    before it extracts a historical provider -- and the Scripted harness
    executes that guard for real against ROOT, so in the OVERLAY world every
    scripted Driver test died at source-guard-native-versions; only there,
    because with the overlay absent the same guard answers its pinned 77
    before it ever needs a temp area, which is why the absent world never saw
    this. A real guard therefore runs with a temp area its own contract
    allows: the environment's own when it already lies outside the tree (a
    dev shell's TMPDIR under /var/folders), else the platform temp dir the
    row's pin replaced. The row's pinned TMPDIR still governs the harness's
    own tamper trees, which never leave the build dir.
    """
    repository = ROOT.resolve()
    current = Path(tempfile.gettempdir()).resolve()
    if current != repository and repository not in current.parents:
        return {'TMPDIR': str(current)}
    saved = tempfile.tempdir
    masked = {name: os.environ.pop(name)
              for name in ('TMPDIR', 'TEMP', 'TMP') if name in os.environ}
    try:
        tempfile.tempdir = None
        return {'TMPDIR': tempfile.gettempdir()}
    finally:
        tempfile.tempdir = saved
        os.environ.update(masked)


def setUpModule():
    # The module describes the tree it runs on: the fixture-absent public
    # world. PINEFORGE_PRIVATE_FIXTURES must be absent for it, because the
    # Scripted harness executes the real source guards, and a real guard
    # whose private input the tree lacks exits 77 -- accepted only while the
    # overlay is absent (R2-F3). The overlay-present controls set the flag
    # themselves (overlay_present / RowFloorDriver).
    global _PRIOR_OVERLAY_FLAG
    _PRIOR_OVERLAY_FLAG = os.environ.get('PINEFORGE_PRIVATE_FIXTURES', _SENTINEL)
    os.environ.pop('PINEFORGE_PRIVATE_FIXTURES', None)


def tearDownModule():
    global _PRIOR_OVERLAY_FLAG
    if _PRIOR_OVERLAY_FLAG is not _SENTINEL:
        os.environ['PINEFORGE_PRIVATE_FIXTURES'] = str(_PRIOR_OVERLAY_FLAG)


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


def ctest_output(total: int, skipped=(), not_run=(), disabled=()) -> bytes:
    """CTest 4.4's console shape for a run of `total` rows, as ci_verify reads it.

    Each skipped or not-run row prints its own result line during the run and
    is listed again after the closing summary, which counts it inside `total`
    (a skipped row as passed, a not-run row as failed). A disabled row is
    listed the same way but CTest leaves it out of `total`.
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
    if skipped or disabled:
        lines.append('\nThe following tests did not run:')
        lines += [f'\t{number:3} - {name} (Skipped)' for number, name in enumerate(skipped, 1)]
        lines += [f'\t{number:3} - {name} (Disabled)'
                  for number, name in enumerate(disabled, len(skipped) + 1)]
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
        # Include discovery calls when checking every profile's dispatch.
        # The runner signature rejects any deadline keyword.
        self.invocations: list[list[str]] = []
        self.cxx = shutil.which('c++') or '/usr/bin/c++'
        self.secret_seen_in_argv = False

    def __call__(self, argv, *, extra_env=None, combine_stderr=True,
                 stream_output=False) -> Completed:
        argv = list(map(str, argv))
        if argv[0] == 'setarch':
            if argv[2] != '-R':
                raise AssertionError('ThreadSanitizer must disable ASLR only for its child')
            argv = argv[3:]
        self.invocations.append(argv)
        # Discovery invocations are asserted through Driver stages. Keep the
        # existing execution-call fixture stable for pre-existing tests.
        if argv[0] != 'ctest' or ('-N' not in argv and '--show-only=json-v1' not in argv):
            self.calls.append(argv)
        extra_env = extra_env or {}
        joined = ' '.join(argv)
        secret = os.environ.get('CI_VERIFY_TEST_SECRET')
        if secret and secret in joined:
            self.secret_seen_in_argv = True
        if argv[0] == 'ctest' and '--help' in argv:
            if self.exits.get('junit_help') == 'absent':
                return Completed(0, b'CTest help without junit\n', b'')
            return default_runner(argv, extra_env=None,
                                  combine_stderr=True, stream_output=False)
        if argv[0] in {'cmake', 'ctest', 'git'} and '--version' in argv:
            return default_runner(argv, extra_env=None,
                                  combine_stderr=True, stream_output=False)
        if argv[0] == 'git' and 'submodule' in argv:
            if 'update' in argv:
                return Completed(int(self.exits.get('corpus-submodule-init', 0)),
                                 b'corpus initialized\n', b'')
            if 'status' in argv:
                prefix = str(self.exits.get('corpus-submodule-prefix', ' '))
                return Completed(0, (prefix + 'b46cd80c247a53b19e23cb0c12c4451d624ce9a6 corpus\n').encode(), b'')
        if argv[0] == 'git' and 'lfs' in argv:
            return Completed(int(self.exits.get('corpus-live-feed', 0)), b'corpus LFS feed\n', b'')
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
            if self.exits.get(key) == 'skip-77':
                # The absent-overlay shape: the preparer answers a missing
                # private-fixture manifest with the pinned literal and exit 77.
                return Completed(77, b'SKIP private fixtures not present: '
                                 b'tests/fixtures/native_cpp_abi/host-f736676/manifest.json\n', b'')
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
                    if self.exits[key] == 'skip-77':
                        # The absent-overlay shape: exit 77 with the pinned
                        # literal naming the missing fixture path.
                        return Completed(77,
                                         b'SKIP private fixtures not present: '
                                         b'tests/fixtures/native_cpp_abi/host-fixture/headers.tar\n',
                                         b'')
                    return Completed(int(self.exits[key]), b'', b'injected source-guard failure\n')
            return default_runner(argv, extra_env=guard_environment(),
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
            if '--show-only=json-v1' in argv:
                names = {'native_live_help', 'test_live_json',
                         'test_native_live_startup', 'test_native_live_store',
                         'test_native_live_routing',
                         'test_native_example_batch', 'test_native_example_selected'}
                if (self.source / 'runner' / 'transport.cpp').is_file():
                    names.add('test_native_live_websocket')
                names.update(ci_verify.runner_e2e_test_names(self.source))
                names.discard(self.exits.get('live_missing_test'))
                return Completed(0, json.dumps({'tests': [{'name': name}
                                  for name in sorted(names)]}).encode(), b'')
            if self.exits.get('actual_empty_ctest'):
                return default_runner(argv, extra_env=extra_env,
                                      combine_stderr=combine_stderr, stream_output=False)
            registered = self.exits.get(
                'ctest_registered',
                ci_verify.EXCLUDED_REGISTERED_MIN.get(
                    self.profile, ci_verify.profile_min_tests(
                        ci_verify.PROFILE[self.profile], self.source) or KERNEL_MIN_TESTS))
            labelled = self.exits.get('ctest_labelled', 5)
            if '-N' in argv:
                stage = 'ctest-list-selected' if '-LE' in argv else 'ctest-list-all'
                if self.exits.get(stage) == 'unreadable':
                    return Completed(0, b'CTest list has no total\n', b'')
                if self.exits.get(stage) == 'names-less':
                    # A listing whose count parses but carries no row names:
                    # the registry the pinned-rows-that-ran rule reads.
                    count = registered - labelled if '-LE' in argv else registered
                    return Completed(0, f'Total Tests: {count}\n'.encode(), b'')
                count = registered - labelled if '-LE' in argv else registered
                # Every row a run scripts as skipped/not-run/disabled is
                # registered by its own row, exactly as a real `ctest -N`
                # lists it; the rest of the registry is filler names.
                names = list(self.exits.get('ctest_registered_names', ()))
                for implied in (self.exits.get('ctest_skipped', ()),
                                self.exits.get('ctest_not_run', ()),
                                self.exits.get('ctest_disabled', ())):
                    names += [name for name in implied if name not in names]
                while len(names) < count:
                    names.append(f'test_row_{len(names) + 1}')
                listing = ''.join(f'Test #{number}: {name}\n'
                                  for number, name in enumerate(names[:count], 1))
                return Completed(int(self.exits.get(stage, 0)),
                                 (listing + f'Total Tests: {count}\n').encode(), b'')
            env_ok = True
            if ci_verify.PROFILE[self.profile].sanitizers:
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
            default_rows = (registered - labelled if '-LE' in argv else
                            ci_verify.profile_min_tests(
                                ci_verify.PROFILE[self.profile], self.source) or KERNEL_MIN_TESTS)
            rows = self.exits.get('ctest_rows', default_rows)
            if rows == 'absent':
                return Completed(int(self.exits.get('ctest', 0)), b'tests\n', b'')
            summary = ctest_output(rows, self.exits.get('ctest_skipped', ()),
                                   self.exits.get('ctest_not_run', ()),
                                   self.exits.get('ctest_disabled', ()))
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
        profile = ci_verify.PROFILE[self.profile]
        tutorial = 'ON' if profile.tutorial else 'OFF'
        live = 'ON' if profile.live_runner else 'OFF'
        sanitizers = 'ON' if profile.sanitizers else 'OFF'
        source_layer = 'OFF' if self.profile == 'kernel' else 'ON'
        examples = 'ON' if self.profile in {'release', 'kernel'} else 'OFF'
        build_type = profile.build_type
        values = {
            'CMAKE_HOME_DIRECTORY': str(self.source),
            'CMAKE_CXX_COMPILER': self.cxx,
            'CMAKE_C_COMPILER': self.cxx,
            'CMAKE_GENERATOR': 'Unix Makefiles',
            'CMAKE_BUILD_TYPE': build_type,
            'PINEFORGE_BUILD_TESTS': 'ON',
            'PINEFORGE_BUILD_TUTORIAL': tutorial,
            'PINEFORGE_BUILD_LIVE_RUNNER': live,
            'PINEFORGE_LIVE_TSAN': 'ON' if ci_verify.PROFILE[self.profile].thread_sanitizers else 'OFF',
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
            commands = (self._library_compile_commands() + self._example_compile_commands()
                        + self._runner_compile_commands())
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
        if (ci_verify.PROFILE[self.profile].sanitizers and
                self.exits.get('sanitizer_flag') != 'absent'):
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
            if ci_verify.PROFILE[self.profile].thread_sanitizers:
                extra += ' -fsanitize=thread -fno-omit-frame-pointer'
            elif ci_verify.PROFILE[self.profile].sanitizers:
                extra += ' ' + SANITIZER_FLAG + ' -fno-omit-frame-pointer'
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

    def _runner_compile_commands(self) -> list[dict]:
        if self.profile not in ci_verify.LIVE_ONLY_PROFILES:
            return []
        commands = []
        for unit in ci_verify.runner_translation_units(self.source):
            if self.exits.get('runner_command_missing') == unit.name:
                continue
            sanitizer = '-fsanitize=thread' if self.profile == 'live-tsan' else SANITIZER_FLAG
            flags = sanitizer + ' -fno-omit-frame-pointer'
            if self.exits.get('runner_flag_missing') == unit.name:
                flags = '-fno-omit-frame-pointer'
            obj = f'runner/CMakeFiles/runner_{unit.stem}.dir/{unit.name}.o'
            commands.append({'directory': str(self.build_dir), 'file': str(unit),
                             'output': obj,
                             'arguments': [self.cxx, *flags.split(), '-o', obj, '-c', str(unit)]})
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
        if ci_verify.PROFILE[self.profile].live_runner or self.exits.get('create_native_binaries'):
            binary = self.build_dir / 'bin' / 'pineforge-live'
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_bytes(b'live')
            if (self.source / 'runner' / 'transport.cpp').is_file():
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
        if ci_verify.PROFILE[self.profile].live_runner:
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
        # The two campaign profiles carry the FULL overlay-present floor from
        # PRIVATE_FIXTURE_PROFILE_FLOORS; the module runs with the overlay
        # present, so that is the value validate_config resolves. --min-tests
        # replaces it verbatim.
        for name in ('kernel', 'release'):
            with self.subTest(profile=name):
                self.assertEqual(ci_verify.PROFILE[name].min_tests, FULL_FLOOR[name])
                # The module runs in the fixture-absent world: the effective
                # default is the table's absent floor. With the overlay set it
                # is the full population (proved in PrivateFixtureFloors).
                self.assertEqual(validate_config(parse_args([name])).min_tests,
                                 ABSENT_FLOOR[name])
                with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'}):
                    self.assertEqual(validate_config(parse_args([name])).min_tests,
                                     FULL_FLOOR[name])
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
                 '--parallel', '2'], stream_output=False)
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

    def test_live_sanitizers_enable_runner_and_gate_websockets_without_opt_in(self):
        values, _ = self.definitions('live-sanitizers')
        self.assertEqual(values['CMAKE_BUILD_TYPE'], 'Debug')
        self.assertEqual(values['PINEFORGE_BUILD_LIVE_RUNNER'], 'ON')
        self.assertEqual(values['PINEFORGE_ENABLE_SANITIZERS'], 'ON')
        self.assertEqual(values['PINEFORGE_BUILD_SOURCE_LAYER'], 'ON')
        self.assertEqual(values['PINEFORGE_BUILD_TUTORIAL'], 'OFF')
        cfg = validate_config(parse_args(['live-sanitizers'], source=ROOT))
        present = (ROOT / 'runner' / 'transport.cpp').is_file()
        self.assertEqual(cfg.require_websocket, present)
        self.assertEqual(cfg.min_tests, LIVE_SANITIZERS_MIN_TESTS - int(not present))
        with self.assertRaisesRegex(ConfigError, 'must run every runner row'):
            self.definitions('live-sanitizers', ['--exclude-label', 'slow'])

    def test_live_sanitizers_config_follows_transport_presence_and_preserves_overrides(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'source'
            transport = source / 'runner' / 'transport.cpp'
            transport.parent.mkdir(parents=True)
            for present in (True, False):
                if present:
                    transport.write_text('int transport_fixture;\n')
                else:
                    transport.unlink()
                with self.subTest(transport_present=present):
                    args = parse_args(['live-sanitizers'], source=source)
                    cfg = validate_config(args, source=source)
                    self.assertEqual(cfg.require_websocket, present)
                    self.assertEqual(cfg.min_tests, LIVE_SANITIZERS_MIN_TESTS - int(not present))
                    override = validate_config(parse_args(
                        ['live-sanitizers', '--min-tests', '4', '--require-websocket'],
                        source=source), source=source)
                    self.assertTrue(override.require_websocket)
                    self.assertEqual(override.min_tests, 4)

    def test_live_tsan_is_separate_and_requires_complete_runner_coverage(self):
        values, _ = self.definitions('live-tsan')
        self.assertEqual(values['CMAKE_BUILD_TYPE'], 'Debug')
        self.assertEqual(values['PINEFORGE_BUILD_LIVE_RUNNER'], 'ON')
        self.assertEqual(values['PINEFORGE_LIVE_TSAN'], 'ON')
        self.assertEqual(values['PINEFORGE_ENABLE_SANITIZERS'], 'OFF')
        config = validate_config(parse_args(['live-tsan'], source=ROOT))
        self.assertEqual(config.require_websocket, (ROOT / 'runner' / 'transport.cpp').is_file())
        self.assertEqual(config.min_tests, LIVE_SANITIZERS_MIN_TESTS)
        with self.assertRaisesRegex(ConfigError, 'must run every runner row'):
            self.definitions('live-tsan', ['--exclude-label', 'slow'])
        self.assertIn('native_live_routing_e2e', ci_verify.runner_e2e_test_names(ROOT))
        # Every driver, including corpus equivalence, must be a required row.
        self.assertEqual(ci_verify.runner_e2e_test_names(ROOT),
                         {path.stem for path in (ROOT / 'tests').glob('native_live*_e2e.py')})

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


class RunnerSanitizerCoverage(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.source = root / 'source'
        self.build_dir = root / 'build'
        (self.source / 'runner').mkdir(parents=True)
        self.build_dir.mkdir()
        self.commands = []
        for suffix in ('.c', '.cc', '.cpp', '.cxx'):
            unit = self.source / 'runner' / ('fixture' + suffix)
            unit.write_text('int coverage_fixture;\n')
            self.commands.append(self.command(unit))

    def command(self, unit: Path) -> dict:
        output = f'runner/CMakeFiles/coverage.dir/{unit.name}.o'
        return {'directory': str(self.build_dir), 'file': str(unit), 'output': output,
                'arguments': ['c++', SANITIZER_FLAG, '-fno-omit-frame-pointer',
                              '-o', output, '-c', str(unit)]}

    def coverage(self, commands: list[dict]) -> list[str]:
        (self.build_dir / 'compile_commands.json').write_text(json.dumps(commands))
        return ci_verify.runner_sanitizer_coverage(self.build_dir, self.source)

    def test_all_c_and_cxx_suffixes_require_compile_commands(self):
        self.assertEqual(len(self.coverage(self.commands)), len(self.commands))
        for command in self.commands:
            with self.subTest(unit=command['file']):
                without_unit = [entry for entry in self.commands if entry is not command]
                with self.assertRaisesRegex(RuntimeError, re.escape(Path(command['file']).name)):
                    self.coverage(without_unit)

    def test_out_of_tree_units_are_instrumented_and_reported(self):
        generated = self.build_dir / 'generated.cc'
        generated.write_text('int generated_fixture;\n')
        command = self.command(generated)
        coverage = self.coverage([*self.commands, command])
        self.assertIn(f'coverage: {generated.resolve()} (outside source tree)', coverage)
        command['arguments'].remove(SANITIZER_FLAG)
        with self.assertRaisesRegex(RuntimeError, 'outside source tree'):
            self.coverage([*self.commands, command])

    def test_sanitizer_and_frame_pointer_opt_out_flags_are_rejected(self):
        for flag in ('-fno-sanitize=address', '-fno-sanitize=undefined',
                     '-fno-sanitize=all', '-fomit-frame-pointer'):
            with self.subTest(flag=flag):
                command = dict(self.commands[0])
                command['arguments'] = [*command['arguments'], flag]
                with self.assertRaisesRegex(RuntimeError, 'lacks ASan/UBSan/frame pointers'):
                    self.coverage([command, *self.commands[1:]])


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


class HistoricalProviderPinTable(unittest.TestCase):
    """The provider pin table itself -- no archive is read, so both worlds run it.

    R2-F11: the replaced skip keyed the whole class on a directory probe, which
    also silenced assertions that need no private data. The pins below hold in
    the fixture-absent tree exactly as in the overlay one; only the closure
    digests and the authentication controls are gated (on the overlay flag, in
    HistoricalProviderPins).
    """

    def test_all_historical_roles_pin_their_own_engine_epoch(self):
        self.assertEqual({role: provider['engine_epoch'] for role, provider in PROVIDERS.items()}, {
            'e60': 'engine_script_run_v13', '0e': 'engine_script_run_v13',
            'v13': 'engine_script_run_v13', 'v14': 'engine_script_run_v14',
            'v15-frozen': 'engine_script_run_v15',
            'v16-frozen': 'engine_script_run_v16',
            'v18-frozen': 'engine_script_run_v18',
        })

    def test_each_host_provider_pins_commit_tree_and_closure_paths(self):
        for role, commit, tree, host, output in (
                ('v14', V14_COMMIT, V14_TREE, 'host-f736676', 'native-abi-v14'),
                ('v15-frozen', V15_FROZEN_COMMIT, V15_FROZEN_TREE, 'host-e7cdf05',
                 'native-abi-v15-frozen'),
                ('v16-frozen', V16_FROZEN_COMMIT, V16_FROZEN_TREE, 'host-ab9714b',
                 'native-abi-v16-frozen'),
                ('v18-frozen', V18_FROZEN_COMMIT, V18_FROZEN_TREE, 'host-fc7aad6',
                 'native-abi-v18-frozen')):
            with self.subTest(role=role):
                provider = PROVIDERS[role]
                self.assertEqual(provider['commit'], commit)
                self.assertEqual(provider['tree'], tree)
                self.assertEqual(provider['manifest'],
                                 ROOT / 'tests/fixtures/native_cpp_abi' / host / 'manifest.json')
                self.assertEqual(provider['default_output'], output)
                self.assertEqual(provider['headers_name'], 'headers.tar')


# The overlay FLAG, not a directory probe, decides whether the host-* header
# closures are here (R2-F11): under the campaign a tree that carries the files
# but runs without the flag is the absent world's job, and one that runs with
# the flag must fail loudly if the closures are missing rather than skip.
requires_private_closures = unittest.skipUnless(
    ci_verify.private_fixtures_overlay(),
    'private fixtures not present: the host-* header closures are '
    'moved out of the public tree (tests/private_fixture_rows.txt)')


@requires_private_closures
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
            RUNTIME_HEADERS,
            _pf_api_names,
        )
        # The public count is read the way the script reads it: pineforge.h
        # plus the two additive v1 headers.
        header = [name for relative in RUNTIME_HEADERS
                  for name in _pf_api_names(ROOT / 'include/pineforge' / relative)]
        runtime = _pf_api_names(ROOT / 'src/c_abi.cpp')
        self.assertEqual(EXPECTED_PUBLIC_DECLARATIONS, 100)
        self.assertEqual(EXPECTED_RUNTIME_IMPLEMENTATIONS, 80)
        self.assertEqual(len(EXPECTED_RUNTIME), EXPECTED_RUNTIME_IMPLEMENTATIONS)
        self.assertEqual(len(header), EXPECTED_PUBLIC_DECLARATIONS)
        self.assertEqual(len(header), len(set(header)))
        self.assertEqual(len(runtime), EXPECTED_RUNTIME_IMPLEMENTATIONS)
        self.assertEqual(set(runtime), EXPECTED_RUNTIME)
        self.assertIn('strategy_configure_native_fx_curve_v1', header)
        self.assertIn('strategy_configure_native_fx_curve_v1', runtime)
        self.assertIn('strategy_configure_native_fx_curve_ext_v1', header)
        self.assertIn('strategy_configure_native_fx_curve_ext_v1', runtime)
        for name in ('strategy_set_symbol_feed', 'strategy_set_symbol_feed_column',
                     'strategy_set_symbol_facts', 'strategy_set_recorded_series'):
            self.assertIn(name, header)
            self.assertIn(name, runtime)
        # Generated, only by a script declaring use_bar_magnifier = true.
        self.assertIn('strategy_declares_bar_magnifier', header)
        self.assertNotIn('strategy_declares_bar_magnifier', runtime)
        for name in ('strategy_capabilities_api_version', 'strategy_capabilities_receipt'):
            self.assertIn(name, header)
            self.assertNotIn(name, runtime)

    def test_historical_only_setters_say_a_stream_refuses_them(self):
        from check_c_abi_runtime import (
            HISTORICAL_ONLY_PHRASE,
            HISTORICAL_ONLY_RUNTIME,
            _historical_only_undocumented,
        )
        text = (ROOT / 'include/pineforge/pineforge.h').read_text(encoding='utf-8')
        self.assertEqual(_historical_only_undocumented(text), [])
        self.assertIn('strategy_set_symbol_feed', HISTORICAL_ONLY_RUNTIME)
        # Mutation: the phrase dropped from one setter's own doc comment is named.
        head, _, tail = text.partition('PF_API int strategy_set_recorded_series(')
        cut = head.rsplit(HISTORICAL_ONLY_PHRASE, 1)
        mutated = cut[0] + 'stream_begin() accepts it' + cut[1] \
            + 'PF_API int strategy_set_recorded_series(' + tail
        self.assertEqual(_historical_only_undocumented(mutated),
                         ['strategy_set_recorded_series'])


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
                result = default_runner(argv, extra_env=guard_environment(),
                                        combine_stderr=True, stream_output=False)
                if result.returncode == 77:
                    # A guard whose private fixture input the tree lacks skips
                    # with the pinned literal naming the missing path -- and
                    # only that: the named file must really be absent.
                    output = (result.stdout + result.stderr).decode('utf-8', 'replace')
                    literal = ci_verify.PRIVATE_FIXTURE_SKIP_LITERAL
                    self.assertIn(literal, output, output[-2000:])
                    named = output.split(literal, 1)[1].strip().splitlines()[0]
                    self.assertGreater(len(named), 0, 'the skip must name a path')
                    self.assertFalse((ROOT / named).exists(),
                                     named + ' exists, so the skip is not earned')
                    continue
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

    def test_live_corpus_lfs_failure_stops_before_guards_and_configure(self):
        code, summary, scripted, _ = self.run_profile(
            'live-sanitizers', **{'corpus-live-feed': 1})
        self.assertEqual(code, 1)
        self.assertIn('corpus-live-feed', failure_stages(summary))
        self.assertNotIn('configure', scripted.names())
        self.assertNotIn('source-guard-c-abi', stage_names(summary))

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

    def test_source_guard_77_without_the_literal_is_a_failure(self):
        # The private-fixture skip is the pinned literal; a bare 77 is not one.
        code, summary, scripted, _ = self.run_profile(**{'source-guard-c-abi': 77})
        self.assertEqual(code, 1)
        self.assertIn('source-guard-c-abi', failure_stages(summary))
        self.assertFalse(any(argv[0] == 'cmake' and '-S' in argv for argv in scripted.calls))

    def test_source_guard_77_with_the_pinned_literal_is_a_named_skip(self):
        # GREEN half of the R2-F3 control: with the overlay ABSENT (this
        # module's world) the guard's 77 + literal is the contract and the run
        # goes on.
        code, summary, scripted, _ = self.run_profile(
            **{'source-guard-native-versions': 'skip-77'})
        self.assertEqual(code, 0, summary['failures'])
        stage = next(stage for stage in summary['stages']
                     if stage['name'] == 'source-guard-native-versions')
        self.assertEqual(stage['status'], 'skipped')
        self.assertEqual(stage['exitCode'], 77)
        self.assertFalse(failure_stages(summary))
        self.assertTrue(any(argv[0] == 'cmake' and '-S' in argv for argv in scripted.calls))

    def test_red_overlay_present_a_source_guard_skip_is_refused(self):
        # RED half of the R2-F3 control: with PINEFORGE_PRIVATE_FIXTURES set
        # the guard's private input is in the tree, so the same 77 + literal is
        # a failure -- and configure never runs.
        with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'}):
            code, summary, scripted, _ = self.run_profile(
                **{'source-guard-c-abi': 'skip-77'})
        self.assertEqual(code, 1)
        self.assertIn('source-guard-c-abi', failure_stages(summary))
        stage = next(stage for stage in summary['stages']
                     if stage['name'] == 'source-guard-c-abi')
        self.assertEqual(stage['status'], 'failed')
        self.assertFalse(any(argv[0] == 'cmake' and '-S' in argv for argv in scripted.calls))

    def test_green_overlay_absent_an_abi_preparer_skip_is_a_skip(self):
        # GREEN half of the preparer control: absent the overlay, the
        # receipt-gated provider cannot be prepared and its 77 + literal is a
        # skip the run goes on past.
        code, summary, _, _ = self.run_profile(**{'v14-prepare': 'skip-77'})
        self.assertEqual(code, 0, summary['failures'])
        stage = next(stage for stage in summary['stages'] if stage['name'] == 'abi-v14')
        self.assertEqual(stage['status'], 'skipped')

    def test_red_overlay_present_an_abi_preparer_skip_is_refused(self):
        # RED half: with the overlay present the providers' manifests are in
        # the tree, so a preparer that reports one absent did not receive the
        # overlay -- a failure (R2-F3).
        with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'}):
            # The real fixture-absent guard is scripted out so the run reaches
            # the ABI providers; its own refusal is the control above.
            code, summary, _, _ = self.run_profile(
                **{'v14-prepare': 'skip-77', 'source-guard-native-versions': 0})
        self.assertEqual(code, 1)
        self.assertIn('abi-v14', failure_stages(summary))

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

    def test_slow_native_include_compile_passes_real_verifier_stage(self):
        """Slowness must never fail a required check; takes about two minutes on purpose."""
        from shlex import quote

        compiler = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('C++ compiler not found')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'slow.cpp'
            output = root / 'slow.o'
            depfile = root / 'slow.d'
            source.write_text('int slow_native_include_probe() { return 0; }\n')
            wrapper = root / 'slow-c++'
            wrapper.write_text(
                '#!/bin/sh\nset -e\nsleep 122\n'
                f'exec {quote(compiler)} "$@"\n')
            wrapper.chmod(0o755)
            fixture = root / 'compile_fixture.py'
            fixture.write_text(
                'from pathlib import Path\n'
                'import sys\n'
                'sys.path.insert(0, sys.argv[1])\n'
                'from check_native_include_independence import compile_object\n'
                'root = Path(sys.argv[2])\n'
                'result = compile_object(\n'
                '    compiler=str(root / "slow-c++"), flags=["-std=c++17"],\n'
                '    source=root / "slow.cpp", output=root / "slow.o",\n'
                '    depfile=root / "slow.d", include_root=root,\n'
                '    label="slow native include compile")\n'
                'sys.stdout.write(result.stdout)\n'
                'sys.stderr.write(result.stderr)\n'
                'print(f"compile_object exit code: {result.returncode}", flush=True)\n'
                'raise SystemExit(result.returncode)\n')
            cfg = ci_verify.build_config(
                ['release', '--build-dir', str(root / 'build')],
                runner=default_runner, stream_output=False)
            driver = ci_verify.Driver(cfg)
            result = driver.invoke(
                'native-include-independence',
                [sys.executable, str(fixture), str(ROOT / 'scripts'), str(root)])
            diagnostic = (result.stdout + result.stderr).decode('utf-8', 'replace')
            summary = load_summary(cfg.build_dir)
            stage = summary['stages'][0]
            stage_log = (cfg.build_dir / stage['log']).read_text()
            self.assertEqual(result.returncode, 0, diagnostic)
            self.assertIn('compile_object exit code: 0', diagnostic, diagnostic)
            self.assertTrue(output.is_file(), diagnostic)
            self.assertTrue(depfile.is_file(), diagnostic)
            self.assertEqual(stage['exitCode'], 0, diagnostic)
            self.assertEqual(stage['status'], 'passed', diagnostic)
            self.assertEqual(driver.failures, [], diagnostic)
            self.assertEqual(summary['failures'], [], diagnostic)
            for captured in (diagnostic, stage_log):
                for forbidden in ('TIMEOUT', 'TimeoutExpired', 'timed out'):
                    self.assertNotIn(forbidden, captured, diagnostic)
                self.assertNotRegex(captured, r'(?i)\bdeadline\b|\btime\s*limit\b',
                                    diagnostic)

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
        # R5 lane H-DOCGATES: the ruled-count floors travel with the argv.
        self.assertEqual(argv[argv.index('--min-ruled-identifiers') + 1],
                         str(ci_verify.ADR_RULED_IDENTIFIERS_MIN))
        self.assertEqual(argv[argv.index('--min-ruled-texts') + 1],
                         str(ci_verify.ADR_RULED_TEXTS_MIN))

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

    def test_pr_exclusion_proves_registered_minus_labelled_equals_ran(self):
        self.assertEqual(ci_verify.EXCLUDED_REGISTERED_MIN,
                         {'debug': 876, 'sanitizers': 867, 'native': 963,
                          'release': 895, 'kernel': 337})
        for profile, registered in ci_verify.EXCLUDED_REGISTERED_MIN.items():
            with self.subTest(profile=profile):
                code, summary, scripted, _ = self.run_profile(
                    profile, extra=['--exclude-label', 'slow'])
                self.assertEqual(code, 0, summary['failures'])
                self.assertEqual(summary['ctestRegistered'], registered)
                self.assertEqual(summary['ctestSelected'], registered - 5)
                self.assertEqual(summary['ctestRows'], registered - 5)
                self.assertIn('ctest-exclusion', stage_names(summary))
                self.assertNotIn('ctest-floor', stage_names(summary))
                self.assertIn('ctest-list-all', stage_names(summary))
                self.assertIn('ctest-list-selected', stage_names(summary))

    def test_no_profile_bounds_a_stage_by_default(self):
        # Owner rule, 2026-10-12: no time limit of ours may fail a run for
        # slowness. The scripted runner rejects deadline keywords, and CTest
        # receives an explicit zero default per-test timeout.
        for profile, extra in (('sanitizers', []),
                               ('sanitizers', ['--exclude-label', 'slow']),
                               ('debug', []), ('debug', ['--exclude-label', 'slow']),
                               ('release', []), ('kernel', []),
                               ('native', []), ('live-sanitizers', []), ('live-tsan', [])):
            with self.subTest(profile=profile, extra=extra):
                code, summary, scripted, _ = self.run_profile(profile, extra=extra)
                self.assertEqual(code, 0, summary['failures'])
                self.assertTrue(scripted.invocations)
                self.assertNotIn('testTimeoutSeconds', summary)
                self.assertNotIn('ctestTimeoutSeconds', summary)
                ctest_argv = next(argv for argv in scripted.invocations
                                  if argv[0] == 'ctest' and '--output-on-failure' in argv)
                self.assertEqual(ctest_argv[ctest_argv.index('--timeout') + 1], '0')

    def test_ctest_discovery_count_rejects_missing_or_ambiguous_summary(self):
        self.assertEqual(ci_verify.ctest_list_count(b'Test #1: a\nTotal Tests: 1\n'), 1)
        self.assertIsNone(ci_verify.ctest_list_count(b'No tests were found\n'))
        self.assertIsNone(ci_verify.ctest_list_count(b'Total Tests: 1\nTotal Tests: 2\n'))

    def test_pr_exclusion_refuses_lost_registration_or_label(self):
        for exits in ({'ctest_registered': 652}, {'ctest_labelled': 0},
                      {'ctest_rows': 647}, {'ctest_skipped': ['silent_skip']},
                      {'ctest-list-selected': 'unreadable'}):
            with self.subTest(exits=exits):
                code, summary, _, _ = self.run_profile(
                    'debug', extra=['--exclude-label', 'slow'], **exits)
                self.assertEqual(code, 1)
                self.assertIn('ctest-exclusion', failure_stages(summary))

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
        self.assertEqual(summary['ctestRegistered'], ci_verify.EXCLUDED_REGISTERED_MIN['native'])
        self.assertEqual(summary['ctestSelected'], summary['ctestRegistered'])
        self.assertEqual(summary['ctestSkipped'], [])

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

    def test_live_sanitizers_follow_transport_presence_and_run_e2es_under_strict_env(self):
        original_is_file = Path.is_file
        for present in (True, False):
            def is_file(path):
                if path == ROOT / 'runner' / 'transport.cpp':
                    return present
                return original_is_file(path)
            with self.subTest(transport_present=present), mock.patch.object(
                    Path, 'is_file', autospec=True, side_effect=is_file):
                code, summary, scripted, build_dir = self.run_profile('live-sanitizers')
                self.assertEqual(code, 0, summary['failures'])
                self.assertIn('live-sanitizer-coverage', stage_names(summary))
                self.assertIn('live-test-inventory-required', stage_names(summary))
                self.assertIn('abi-providers-skipped', stage_names(summary))
                self.assertEqual(summary['requireWebsocket'], present)
                self.assertEqual(summary['minTests'], LIVE_SANITIZERS_MIN_TESTS - int(not present))
                self.assertEqual(summary['ctestRows'], summary['minTests'])
                self.assertEqual(summary['ctestRegistered'], summary['minTests'])
                self.assertEqual(summary['ctestSelected'], summary['minTests'])
                inventory = next(stage for stage in summary['stages']
                                 if stage['name'] == 'live-test-inventory-required')
                inventory_log = (build_dir / inventory['log']).read_text()
                self.assertEqual('test_native_live_websocket' in inventory_log, present)
                self.assertEqual('require-websocket' in stage_names(summary), present)
                self.assertEqual((build_dir / 'bin' / 'test_native_live_websocket').is_file(), present)
                ctest = next(stage for stage in summary['stages'] if stage['name'] == 'ctest')
                self.assertIn(str((build_dir / 'runner').resolve()), ctest['argv'])
                for name in ('ctest', 'native-help', *(['require-websocket'] if present else [])):
                    stage = next(stage for stage in summary['stages'] if stage['name'] == name)
                    self.assertEqual(stage['extraEnvKeys'], ['ASAN_OPTIONS', 'UBSAN_OPTIONS'])
                self.assertNotIn('abi-base-prepare', scripted.names())
                code, below_floor, _, _ = self.run_profile(
                    'live-sanitizers', ctest_rows=summary['minTests'] - 1)
                self.assertEqual(code, 1)
                self.assertIn('ctest-floor', failure_stages(below_floor))

    def test_live_sanitizers_refuse_an_uninstrumented_or_missing_runner_compile(self):
        for unit in ci_verify.runner_translation_units(ROOT):
            for key in ('runner_flag_missing', 'runner_command_missing'):
                with self.subTest(unit=unit, defect=key):
                    code, summary, scripted, _ = self.run_profile('live-sanitizers', **{key: unit.name})
                    self.assertEqual(code, 1)
                    self.assertIn('live-sanitizer-coverage', failure_stages(summary))
                    self.assertNotIn('build', scripted.names())

    def test_live_tsan_runs_the_same_inventory_and_catches_missing_instrumentation(self):
        code, summary, scripted, _ = self.run_profile('live-tsan')
        self.assertEqual(code, 0, summary['failures'])
        self.assertIn('runner-thread-sanitizer-coverage', stage_names(summary))
        self.assertIn('live-test-inventory-required', stage_names(summary))
        self.assertEqual(summary['ctestRegistered'], LIVE_SANITIZERS_MIN_TESTS)
        self.assertEqual(summary['ctestSelected'], LIVE_SANITIZERS_MIN_TESTS)
        self.assertEqual(summary['ctestSkipped'], [])
        self.assertNotIn('sanitizer-public-flag', stage_names(summary))
        self.assertNotIn('live-sanitizer-coverage', stage_names(summary))
        for name in ('ctest', 'native-help', 'require-websocket'):
            stage = next(stage for stage in summary['stages'] if stage['name'] == name)
            self.assertEqual(stage['extraEnvKeys'], ['TSAN_OPTIONS'])
            self.assertEqual(stage['argv'][:3], ['setarch', os.uname().machine, '-R'])
        for unit in ci_verify.runner_translation_units(ROOT):
            for key in ('runner_flag_missing', 'runner_command_missing'):
                with self.subTest(unit=unit.name, defect=key):
                    code, summary, scripted, _ = self.run_profile('live-tsan', **{key: unit.name})
                    self.assertEqual(code, 1)
                    self.assertIn('runner-thread-sanitizer-coverage', failure_stages(summary))
                    self.assertNotIn('build', scripted.names())

    def test_live_sanitizers_refuse_a_missing_required_row(self):
        names = ci_verify.runner_e2e_test_names(ROOT)
        if (ROOT / 'runner' / 'transport.cpp').is_file():
            names.add('test_native_live_websocket')
        for name in sorted(names):
            with self.subTest(row=name):
                code, summary, scripted, _ = self.run_profile(
                    'live-sanitizers', live_missing_test=name)
                self.assertEqual(code, 1)
                self.assertIn('live-test-inventory-required', failure_stages(summary))
                self.assertNotIn('ctest', scripted.names())

    def test_live_sanitizers_refuse_skips_and_unsupported_websockets(self):
        cases = [({'ctest_rows': LIVE_SANITIZERS_MIN_TESTS + 1,
                   'ctest_skipped': ['native_live_e2e']}, 'live-test-skips')]
        if (ROOT / 'runner' / 'transport.cpp').is_file():
            cases.append(({'require-websocket': 77}, 'require-websocket-skip'))
        for defects, stage in cases:
            with self.subTest(defects=defects):
                code, summary, _, _ = self.run_profile('live-sanitizers', **defects)
                self.assertEqual(code, 1)
                self.assertIn(stage, failure_stages(summary))

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
        help_text = default_runner(['ctest', '--help'], extra_env=None,
                                   combine_stderr=True, stream_output=False)
        self.assertEqual(help_text.returncode, 0)
        self.assertEqual(ctest_supports_junit(default_runner),
                         '--output-junit' in help_text.stdout.decode())


class RowFloorDriver(unittest.TestCase):
    """The floor stage in the OVERLAY-PRESENT world:

    the full PRIVATE_FIXTURE_PROFILE_FLOORS population must run, and a skip
    of any row is a lost row. The absent-world controls live in
    PrivateFixtureFloors, which sets the flag itself.
    """

    def run_profile(self, profile='release', extra=None, **exits):
        exits.setdefault('source-guard-native-versions', 0)
        with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'}):
            return DriverOrderingAndAggregation.run_profile(self, profile, extra=extra, **exits)

    def test_kernel_floor_passes_when_ctest_reports_enough_rows(self):
        code, summary, _, _ = self.run_profile('kernel')
        self.assertEqual(code, 0, summary['failures'])
        names = stage_names(summary)
        self.assertIn('ctest-floor', names)
        self.assertLess(names.index('ctest'), names.index('ctest-floor'))
        self.assertLess(names.index('ctest-floor'), names.index('install'))
        self.assertEqual(summary['minTests'], FULL_FLOOR['kernel'])
        self.assertEqual(summary['ctestRows'], FULL_FLOOR['kernel'])

    def test_kernel_floor_fails_below_it_without_hiding_package_checks(self):
        code, summary, scripted, _ = self.run_profile('kernel', ctest_rows=FULL_FLOOR['kernel'] - 1)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertNotIn('ctest', failure_stages(summary))
        self.assertIn('install', scripted.names())
        self.assertIn('smoke-version', scripted.names())
        error = next(item['error'] for item in summary['failures'] if item['stage'] == 'ctest-floor')
        self.assertIn(str(FULL_FLOOR['kernel'] - 1), error)
        self.assertIn(str(FULL_FLOOR['kernel']), error)
        self.assertEqual(summary['ctestRows'], FULL_FLOOR['kernel'] - 1)

    def test_kernel_floor_fails_closed_without_a_row_count(self):
        code, summary, _, _ = self.run_profile('kernel', ctest_rows='absent')
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertIsNone(summary['ctestRows'])

    def test_release_floor_fails_below_it_without_hiding_package_checks(self):
        code, summary, scripted, _ = self.run_profile('release', ctest_rows=FULL_FLOOR['release'] - 1)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertNotIn('ctest', failure_stages(summary))
        self.assertIn('install', scripted.names())
        self.assertIn('smoke-version', scripted.names())
        error = next(item['error'] for item in summary['failures'] if item['stage'] == 'ctest-floor')
        self.assertIn(str(FULL_FLOOR['release'] - 1), error)
        self.assertIn(str(FULL_FLOOR['release']), error)
        self.assertIn('release profile', error)
        self.assertEqual(summary['minTests'], FULL_FLOOR['release'])

    def test_release_floor_passes_at_it_and_fails_closed_without_a_row_count(self):
        code, summary, _, _ = self.run_profile('release')
        self.assertEqual(code, 0, summary['failures'])
        names = stage_names(summary)
        self.assertLess(names.index('ctest'), names.index('ctest-floor'))
        self.assertEqual(summary['ctestRows'], FULL_FLOOR['release'])
        code, summary, _, _ = self.run_profile('release', ctest_rows='absent')
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))

    def test_floor_counts_rows_that_ran_and_lists_a_skipped_row_beside_them(self):
        # The floor counts the rows that RAN: one row more registered than the
        # floor, and that row skipped, leaves the ran count exactly at the
        # floor -- which passes -- with the skip listed beside the count.
        code, summary, _, build_dir = self.run_profile(
            'kernel', ctest_rows=FULL_FLOOR['kernel'] + 1,
            ctest_skipped=['test_native_live_websocket'])
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['ctestRows'], FULL_FLOOR['kernel'])
        self.assertEqual(summary['ctestTotal'], FULL_FLOOR['kernel'] + 1)
        self.assertEqual(summary['ctestSkipped'], ['test_native_live_websocket'])
        self.assertEqual(summary['ctestNotRun'], [])
        self.assertEqual(summary['ctestDisabled'], [])
        log = (build_dir / 'ci-logs' / 'ctest-floor.log').read_text()
        self.assertIn(f'ctest ran {FULL_FLOOR["kernel"]} rows (floor '
                      f'{FULL_FLOOR["kernel"]})', log)
        self.assertIn('not counted: 1 skipped (test_native_live_websocket)', log)

    def test_a_row_that_stops_running_trips_the_floor_though_ctest_still_counts_it(self):
        # CTest's own count includes a skipped row, so before lane Q12 a row
        # that began to skip left the count at the floor and passed.
        for profile in ('kernel', 'release'):
            floor = FULL_FLOOR[profile]
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
            'kernel', ctest=8, ctest_rows=FULL_FLOOR['kernel'],
            ctest_not_run=['test_missing_binary'])
        self.assertEqual(code, 1)
        self.assertIn('ctest', failure_stages(summary))
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertEqual(summary['ctestRows'], FULL_FLOOR['kernel'] - 1)
        self.assertEqual(summary['ctestNotRun'], ['test_missing_binary'])

    def test_the_floor_still_trips_on_a_real_collapse(self):
        # Every kernel row but one skips, with CTest's count still at the
        # floor; and a release suite that simply shrank.
        skipped = [f'test_row_{number}' for number in range(FULL_FLOOR['kernel'] - 1)]
        code, summary, _, _ = self.run_profile(
            'kernel', ctest_rows=FULL_FLOOR['kernel'], ctest_skipped=skipped)
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

    def overlay_absent(self):
        # Empty is the unset spelling: the flag counts only when non-empty.
        return mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': ''})

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


PRIVATE_FIXTURE_LABEL_CALL = re.compile(
    r'set_tests_properties\(\s*(?P<rows>[^()]*?)\s+PROPERTIES\b[^()]*?\bLABELS\s+'
    + re.escape(ci_verify.PRIVATE_FIXTURE_LABEL) + r'\b')
ROW_VARIABLE = re.compile(r'\$\{([A-Za-z_]\w*)\}')
CMAKE_HEAD = re.compile(r'^\s*([A-Za-z_]\w*)\s*\(')


def _cmake_statements(text: str):
    """Split CMake text into (line number, statement) pairs at balanced parens."""
    statements, buffer, depth, line = [], [], 0, 0
    for number, raw in enumerate(text.splitlines(), 1):
        stripped = raw.strip()
        if not buffer and (not stripped or stripped.startswith('#')):
            continue
        if not buffer:
            line = number
        buffer.append(raw)
        depth += raw.count('(') - raw.count(')')
        if depth <= 0:
            statements.append((line, '\n'.join(buffer)))
            buffer, depth = [], 0
    if buffer:
        statements.append((line, '\n'.join(buffer)))
    return statements


def _statement_arguments(statement: str) -> str:
    return statement[statement.index('(') + 1:statement.rindex(')')]


def _split_values(fragment: str):
    return [token for token in fragment.replace('\n', ' ').split() if token]


def _substitute(token: str, bindings: dict, environment: dict, *, path, line: int, text: str,
                depth: int = 0):
    """One token with every ${variable} resolved from the surrounding walk.

    set() values stay raw in the environment and are resolved here on use, so
    a variable that never reaches a row name cannot fail the walk; one that
    does reach a row name raises ValueError naming file:line -- the walk never
    emits a row name it cannot read.
    """
    def resolve(match):
        name = match.group(1)
        if name in bindings:
            return str(bindings[name])
        if name in environment and depth < 8:
            return ';'.join(_substitute(item, bindings, environment, path=path,
                                        line=line, text=text, depth=depth + 1)
                            for item in environment[name])
        raise ValueError(
            f'{path}:{line}: cannot expand ${{{name}}} in {token!r}; the '
            'private_fixtures label walk does not skip what it cannot read')
    return ROW_VARIABLE.sub(resolve, token)


class _LabelWalk:
    """Expands the row names of LABELS private_fixtures calls, or raises.

    The walk models set(), list(APPEND), foreach (literal / IN ITEMS /
    IN LISTS over a bound list) and if(<bound variable> STREQUAL "<literal>"),
    which are the forms tests/CMakeLists.txt and runner/CMakeLists.txt use to
    bind a labelled row's name. A foreach or include() whose list it cannot
    resolve, or a row name carrying a variable it cannot resolve, raises with
    file:line -- never a silently smaller population. An if() whose condition
    it does not model is walked on BOTH branches: the pinned file is the union
    over both worlds (fixtures present / absent), so union is the right
    reading there, while a STREQUAL on a bound loop variable must pick one
    branch because the branch bodies bind different clock values.
    """

    def __init__(self):
        self.names = set()
        self.path = Path('.')
        self.text = ''
        self.statements = []

    def walk_file(self, path: Path) -> None:
        self.visited = set()
        self._walk_into(Path(path), {}, {})

    def _walk_into(self, path: Path, bindings: dict, environment: dict) -> None:
        """Run one CMake file's statements in the current scope (include() is
        CMake semantics: same scope, and a label inside the included file
        counts exactly like one here)."""
        path = Path(path).resolve()
        if path in self.visited:
            return
        self.visited.add(path)
        previous = self.path, self.text, self.statements
        self.path, self.text = path, path.read_text()
        self.statements = _cmake_statements(self.text)
        self._run(0, len(self.statements), bindings, environment)
        self.path, self.text, self.statements = previous

    def _body_carries_labels(self, start: int, end: int) -> bool:
        return any(ci_verify.PRIVATE_FIXTURE_LABEL in self.statements[index][1]
                   for index in range(start, end))

    def _statement_carries_labels(self, statement: str) -> bool:
        return (statement.lstrip().startswith('set_tests_properties')
                and ci_verify.PRIVATE_FIXTURE_LABEL in statement)

    def _block_bounds(self, index: int):
        """(body_end, else_index, block_end) for the if/foreach at `index`."""
        depth, else_index = 0, None
        for cursor in range(index, len(self.statements)):
            head = CMAKE_HEAD.match(self.statements[cursor][1])
            head = head.group(1) if head else ''
            if head in ('if', 'foreach'):
                depth += 1
            elif head in ('endif', 'endforeach'):
                depth -= 1
                if depth == 0:
                    return cursor, else_index, cursor
            elif head == 'else' and depth == 1:
                else_index = cursor
        raise ValueError(f'{self.path}:{self.statements[index][0]}: unclosed block')

    def _run(self, start: int, end: int, bindings: dict, environment: dict) -> None:
        index = start
        while index < end:
            line, statement = self.statements[index]
            head = CMAKE_HEAD.match(statement)
            head = head.group(1) if head else ''
            if head == 'set':
                values = _split_values(_statement_arguments(statement))
                environment[values[0].strip('"')] = list(values[1:])
                index += 1
            elif head == 'list' and _split_values(_statement_arguments(statement))[:1] == ['APPEND']:
                values = _split_values(_statement_arguments(statement))
                environment.setdefault(values[1].strip('"'), []).extend(values[2:])
                index += 1
            elif head == 'foreach':
                body_end, _, block_end = self._block_bounds(index)
                header = _split_values(_statement_arguments(statement))
                variable, items = None, []
                if len(header) >= 3 and header[1] == 'IN' and header[2] == 'LISTS':
                    variable = header[0]
                    try:
                        for list_name in header[3:]:
                            items.extend(environment[list_name])
                    except KeyError as error:
                        if self._body_carries_labels(index + 1, body_end):
                            raise ValueError(
                                f'{self.path}:{line}: cannot resolve the foreach list '
                                f'{error.args[0]!r} in a block carrying a '
                                f'{ci_verify.PRIVATE_FIXTURE_LABEL} label') from error
                        index = block_end + 1
                        continue
                elif len(header) >= 2 and header[1] == 'IN' and header[2] == 'ITEMS':
                    variable, items = header[0], header[3:]
                elif header:
                    variable, items = header[0], header[1:]
                else:
                    raise ValueError(f'{self.path}:{line}: unreadable foreach header')
                for item in items:
                    try:
                        bindings[variable] = _substitute(item, bindings, environment,
                                                         path=self.path, line=line,
                                                         text=self.text)
                    except ValueError:
                        if self._body_carries_labels(index + 1, body_end):
                            raise
                        continue
                    self._run(index + 1, body_end, bindings, environment)
                bindings.pop(variable, None)
                index = block_end + 1
            elif head == 'if':
                body_end, else_index, block_end = self._block_bounds(index)
                taken = _condition_value(_statement_arguments(statement), bindings,
                                         environment)
                if self._statement_carries_labels(statement):
                    raise ValueError(f'{self.path}:{line}: unreadable condition {statement!r}')
                then_end = else_index if else_index is not None else body_end
                if taken is True or taken is None:
                    self._run(index + 1, then_end, bindings, environment)
                if taken is False or taken is None:
                    if else_index is not None:
                        self._run(else_index + 1, block_end, bindings, environment)
                index = block_end + 1
            elif head == 'set_tests_properties':
                if ci_verify.PRIVATE_FIXTURE_LABEL in statement:
                    match = PRIVATE_FIXTURE_LABEL_CALL.search(statement)
                    if not match:
                        raise ValueError(
                            f'{self.path}:{line}: a {ci_verify.PRIVATE_FIXTURE_LABEL} '
                            'call in a shape this walk cannot read')
                    for token in _split_values(match.group('rows')):
                        self.names.add(_substitute(token, bindings, environment,
                                                   path=self.path, line=line,
                                                   text=self.text))
                index += 1
            elif head == 'include':
                arguments = _split_values(_statement_arguments(statement))
                if not arguments:
                    raise ValueError(f'{self.path}:{line}: unreadable include()')
                target = arguments[0].strip('"')
                target = target.replace('${PROJECT_SOURCE_DIR}', str(_project_root(self.path)))
                target = target.replace('${CMAKE_CURRENT_SOURCE_DIR}', str(self.path.parent))
                if ROW_VARIABLE.search(target):
                    raise ValueError(
                        f'{self.path}:{line}: cannot resolve {target!r}; the walk '
                        'refuses rather than skipping the include')
                included = Path(target)
                if not included.is_file():
                    raise ValueError(
                        f'{self.path}:{line}: include({target!r}) names no file; '
                        'the walk refuses rather than skipping it')
                self._walk_into(included, bindings, environment)
                index += 1
            else:
                index += 1


def _condition_value(argument: str, bindings: dict, environment: dict):
    """True/False when the condition is decidable on loop bindings, else None.

    The forms the two files use on labelled rows are `VAR STREQUAL "lit"` and
    `VAR MATCHES "re"` (OR-connected) on a bound loop variable; those decide
    exactly which name a per-case branch registers, so they must be evaluated,
    not union-walked. Every other condition (EXISTS/TARGET/flags) depends on
    the world, and there both branches register the SAME row name, so the walk
    takes both (the pinned file is the union over both worlds).
    """
    terms = [term for term in re.split(r'\s+OR\s+', argument.strip()) if term]
    values = []
    for term in terms:
        tokens = _split_values(term)
        value = None
        if len(tokens) == 3 and tokens[1] in ('STREQUAL', 'MATCHES') \
                and tokens[0] in bindings:
            subject, literal = str(bindings[tokens[0]]), tokens[2].strip('"')
            if tokens[1] == 'STREQUAL':
                value = subject == literal
            else:
                value = re.search(literal, subject) is not None
        values.append(value)
    if any(value is True for value in values):
        return True
    return None if any(value is None for value in values) else False


def _project_root(path: Path) -> Path:
    # The two files that register rows sit one directory under the source root.
    return path.parent.parent if path.parent.name in ('tests', 'runner') else path.parent


def labelled_private_fixture_rows(*cmake_paths):
    """The ctest rows a CMake file labels ``private_fixtures``.

    Every ``set_tests_properties(... PROPERTIES ... LABELS private_fixtures)``
    call contributes every row name it lists, expanded through the loops the
    file binds them with (see _LabelWalk). A variable, foreach list or
    include() the walk cannot resolve raises ValueError with file:line -- the
    comparison fails loudly instead of comparing against a silently smaller
    labelled set (the T7 hole: the population was a quiet subset).
    """
    walk = _LabelWalk()
    for path in cmake_paths:
        walk.walk_file(path)
    return walk.names


class PrivateFixtureFloors(unittest.TestCase):
    """The overlay-aware floors and the private_fixtures skip rule.

    PINEFORGE_PRIVATE_FIXTURES selects the world. Set (Lab's overlay is present)
    the full floors apply and a skipped private_fixtures row is a failure;
    unset (public CI) the two RUN floors drop by the pinned private row count
    and the placeholder skips are expected. Each rule has a red and a green
    control here.
    """

    def run_profile(self, profile='release', extra=None, **exits):
        # The Scripted harness executes the real source guards. This tree has
        # no private fixtures, so check_native_cpp_versions genuinely exits 77
        # in BOTH asserted worlds -- accepted while the overlay is absent, a
        # failure under it (R2-F3). The controls here are about the floor and
        # the skip rule, so that one guard is scripted as the world leaves it
        # (it runs and passes), like the ABI preparers; a control that wants
        # its skip passes 'skip-77' explicitly.
        exits.setdefault('source-guard-native-versions', 0)
        return DriverOrderingAndAggregation.run_profile(self, profile, extra=extra, **exits)

    def overlay_present(self):
        return mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'})

    def overlay_absent(self):
        # Empty is the unset spelling: the flag counts only when non-empty.
        return mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': ''})

    def test_pinned_rows_are_exactly_the_labelled_ctest_rows(self):
        # The skip rule reads the rows by their label, and the label's
        # population is the pinned file P1-B commits. Prove the two agree over
        # BOTH files that register rows, so a labelled row cannot be missed and
        # an unlabelled one cannot be refused. The reader expands the literal
        # foreach forms it finds and raises on a row name it cannot resolve, so
        # a label it cannot read fails here instead of shrinking the set
        # silently (the T7 hole: the population must never be a quiet subset).
        labelled = labelled_private_fixture_rows(
            ROOT / 'tests' / 'CMakeLists.txt', ROOT / 'runner' / 'CMakeLists.txt')
        pinned = set(ci_verify.private_fixture_rows(ROOT))
        self.assertEqual(len(pinned), ci_verify.PRIVATE_FIXTURE_ROW_COUNT)
        self.assertEqual(pinned, labelled)

    def test_red_the_comparison_fails_on_a_real_mismatch(self):
        # RED control for the test above: the same reader over tampered copies.
        # A labelled row the pin does not carry, a pinned row whose label is
        # gone, and a row name the reader cannot expand must each surface --
        # an extra name, a missing name, and a loud ValueError -- never a
        # silently equal set.
        tests_text = (ROOT / 'tests' / 'CMakeLists.txt').read_text()
        runner_text = (ROOT / 'runner' / 'CMakeLists.txt').read_text()
        pinned = set(ci_verify.private_fixture_rows(ROOT))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tests').mkdir()
            (root / 'runner').mkdir()
            (root / 'runner' / 'CMakeLists.txt').write_text(runner_text)
            # The walk follows include(); the labs file the runner includes must
            # sit beside the copies, or the walk correctly refuses.
            shutil.copyfile(ROOT / 'tests' / 'confirmed_capabilities_refusals.cmake',
                            root / 'tests' / 'confirmed_capabilities_refusals.cmake')
            label = 'LABELS ' + ci_verify.PRIVATE_FIXTURE_LABEL

            (root / 'tests' / 'CMakeLists.txt').write_text(
                tests_text + '\nset_tests_properties(test_row_not_in_the_pin PROPERTIES '
                + label + ')\n')
            labelled = labelled_private_fixture_rows(
                root / 'tests' / 'CMakeLists.txt', root / 'runner' / 'CMakeLists.txt')
            self.assertIn('test_row_not_in_the_pin', labelled - pinned)
            self.assertNotEqual(pinned, labelled)

            first_label = tests_text.index(label)
            (root / 'tests' / 'CMakeLists.txt').write_text(
                tests_text.replace(label, 'LABELS private_fixture_typo', 1))
            labelled = labelled_private_fixture_rows(
                root / 'tests' / 'CMakeLists.txt', root / 'runner' / 'CMakeLists.txt')
            self.assertTrue(pinned - labelled)
            self.assertNotEqual(pinned, labelled)

            (root / 'tests' / 'CMakeLists.txt').write_text(
                tests_text + '\nset_tests_properties(test_${never_bound_variable} PROPERTIES '
                + label + ')\n')
            with self.assertRaises(ValueError) as caught:
                labelled_private_fixture_rows(
                    root / 'tests' / 'CMakeLists.txt', root / 'runner' / 'CMakeLists.txt')
            self.assertIn('never_bound_variable', str(caught.exception))
            self.assertIn('CMakeLists.txt:', str(caught.exception))
            self.assertIn('tests', str(caught.exception))
            self.assertGreater(first_label, 0)

    def test_the_table_is_the_floor_and_profiles_outside_it_keep_their_own(self):
        # The floors are constants of the profile, read from ONE table; the
        # overlay flag picks between the table's two readings and nothing else
        # feeds them. A profile outside the table keeps its declared value, and
        # PRIVATE_FIXTURE_ROW_COUNT bounds the pinned half by construction.
        self.assertEqual(ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS,
                         {'release': (910, 307), 'kernel': (337, 31)})
        self.assertEqual(FULL_FLOOR, {'release': 910, 'kernel': 337})
        self.assertEqual(ABSENT_FLOOR, {'release': 603, 'kernel': 306})
        for name, (full, pinned) in ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS.items():
            with self.subTest(profile=name):
                self.assertEqual(ci_verify.PROFILE[name].min_tests, full)
                self.assertLessEqual(pinned, ci_verify.PRIVATE_FIXTURE_ROW_COUNT)
        with self.overlay_absent():
            self.assertFalse(ci_verify.private_fixtures_overlay())
            self.assertIsNone(ci_verify.overlay_aware_floor(ci_verify.PROFILE['debug']))
            self.assertIsNone(ci_verify.overlay_aware_floor(ci_verify.PROFILE['native']))
            self.assertEqual(ci_verify.overlay_aware_floor(ci_verify.PROFILE['live-sanitizers']),
                             LIVE_SANITIZERS_MIN_TESTS)
            for name in ('release', 'kernel'):
                self.assertEqual(ci_verify.overlay_aware_floor(ci_verify.PROFILE[name]),
                                 ABSENT_FLOOR[name])
            self.assertEqual(ci_verify.profile_min_tests(ci_verify.PROFILE['kernel'], ROOT),
                             ABSENT_FLOOR['kernel'])
            self.assertEqual(validate_config(parse_args(['release'])).min_tests,
                             ABSENT_FLOOR['release'])
        with self.overlay_present():
            for name in ('release', 'kernel'):
                self.assertEqual(ci_verify.overlay_aware_floor(ci_verify.PROFILE[name]),
                                 FULL_FLOOR[name])
            self.assertEqual(ci_verify.profile_min_tests(ci_verify.PROFILE['release'], ROOT),
                             FULL_FLOOR['release'])
            self.assertEqual(validate_config(parse_args(['kernel'])).min_tests,
                             FULL_FLOOR['kernel'])
        self.assertEqual(ci_verify.EXCLUDED_REGISTERED_MIN,
                         {'debug': 876, 'sanitizers': 867, 'native': 963,
                          'release': 895, 'kernel': 337})

    def test_overlay_flag_moves_the_floor_and_is_the_only_input(self):
        # F9: the flag must MOVE the value. A test that only reads the
        # overlay-present result cannot fail -- the replaced one never passed
        # the observed skips, so it read the full floor whatever the flag said.
        # This one pins both readings against the table in the same test, so
        # dropping the overlay branch (or the table) fails it either way.
        for name in ('release', 'kernel'):
            with self.subTest(profile=name):
                with self.overlay_present():
                    self.assertEqual(ci_verify.overlay_aware_floor(ci_verify.PROFILE[name]),
                                     FULL_FLOOR[name])
                with self.overlay_absent():
                    floor = ci_verify.overlay_aware_floor(ci_verify.PROFILE[name])
                    self.assertNotEqual(floor, FULL_FLOOR[name])
                    self.assertEqual(floor, ABSENT_FLOOR[name])

    def test_green_overlay_present_all_private_rows_run(self):
        # GREEN (flag set): every private_fixtures row runs, so the run passes.
        with self.overlay_present():
            code, summary, _, _ = self.run_profile('release')
        self.assertEqual(code, 0, summary['failures'])
        self.assertIn('ctest-private-fixtures', stage_names(summary))
        self.assertEqual(summary['ctestRows'], FULL_FLOOR['release'])

    def test_red_overlay_present_a_private_row_that_skips_fails(self):
        # RED (flag set): a private_fixtures row that did not run fails, naming
        # the row and the literal it printed. One row more than the floor runs,
        # so only the private_fixtures rule fails -- the floor itself is met.
        with self.overlay_present():
            code, summary, _, _ = self.run_profile(
                'release', ctest_rows=FULL_FLOOR['release'] + 1,
                ctest_skipped=['test_aapl15_margin_brackets_l4a'])
        self.assertEqual(code, 1)
        self.assertEqual(failure_stages(summary), ['ctest-private-fixtures'])
        error = summary['failures'][0]['error']
        self.assertIn('test_aapl15_margin_brackets_l4a', error)
        self.assertIn(ci_verify.PRIVATE_FIXTURE_SKIP_LITERAL, error)
        self.assertIn('the overlay did not reach the tree', error)

    def test_red_overlay_present_unreadable_row_lists_fail_closed(self):
        # F10: with the overlay present and the row lists unreadable, the rule
        # must fail closed -- a floorless profile (debug) has no floor stage to
        # catch it, so a silent return here would pass a run whose CTest output
        # proves nothing.
        with self.overlay_present():
            code, summary, _, _ = self.run_profile(
                'debug', ctest=8, ctest_raw=b'98% tests passed, 3 tests failed out of 999\n')
        self.assertEqual(code, 1)
        self.assertIn('ctest-private-fixtures', failure_stages(summary))

    def test_red_overlay_present_a_disabled_pinned_row_fails(self):
        # F10: a Disabled pinned row is not run either, and CTest leaves it out
        # of its own count, so only this rule can refuse it.
        with self.overlay_present():
            code, summary, _, _ = self.run_profile(
                'debug', ctest_rows=5,
                ctest_disabled=['test_aapl15_margin_brackets_l4a'])
        self.assertEqual(code, 1)
        self.assertIn('ctest-private-fixtures', failure_stages(summary))
        self.assertIn('test_aapl15_margin_brackets_l4a', summary['failures'][0]['error'])

    def pinned_probe(self, count: int) -> list:
        """`count` names that really are in tests/private_fixture_rows.txt."""
        return sorted(ci_verify.private_fixture_rows(ROOT))[:count]

    def test_red_overlay_present_a_truncated_pin_fails_closed(self):
        # F10: a pin file that does not carry the count its own header records
        # cannot speak for the pinned population; the rule refuses it instead
        # of clearing every row it lost. The real Driver method runs against a
        # scratch source whose pin is truncated in half.
        with self.overlay_present(), tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / 'tests').mkdir()
            (source / 'VERSION').write_text('0.0.0\n')
            rows = (ROOT / 'tests' / 'private_fixture_rows.txt').read_text().splitlines()
            (source / 'tests' / 'private_fixture_rows.txt').write_text(
                '\n'.join(rows[:len(rows) // 2]) + '\n')
            cfg = ci_verify.build_config(
                ['kernel', '--build-dir', str(source / 'build')], source=source,
                runner=default_runner, stream_output=False)
            driver = ci_verify.Driver(cfg)
            driver.reject_private_fixture_skips(ci_verify.CTestRows(337))
        self.assertEqual([stage['status'] for stage in driver.stages], ['failed'])
        self.assertIn('a truncated pin cannot clear the rows it lost',
                      driver.stages[0]['error'])

    def test_green_overlay_present_ignores_an_unrelated_skip(self):
        # The rule keys on the pinned rows, not on any skip: a skip outside
        # them must not trip it.
        with self.overlay_present():
            code, summary, _, _ = self.run_profile(
                'kernel', ctest_rows=FULL_FLOOR['kernel'] + 1,
                ctest_skipped=['test_native_live_websocket'])
        self.assertEqual(code, 0, summary['failures'])
        self.assertNotIn('ctest-private-fixtures', failure_stages(summary))

    def absent_run(self, profile, registered, pinned_skips, extra_skips=()):
        """A real Driver run of the absent world with a scripted registry."""
        with self.overlay_absent():
            return self.run_profile(
                profile, ctest_registered=registered, ctest_rows=registered,
                ctest_skipped=list(pinned_skips) + list(extra_skips))

    def full_registration(self, profile) -> int:
        """The profile's overlay-present registered population (the table's `full`)."""
        return FULL_FLOOR[profile]

    def test_red_one_public_row_lost_from_the_registry_fails_the_absent_floor(self):
        # Control (a) of R2-F1: the absent floor is a constant, so a run whose
        # registry loses one public row fails even though `ctest -N` went down
        # with it -- the old second term read the run's own registration and
        # sank with it (the pre-R2 head's release absent run was 603 against
        # a floor of 602 with two of main's rows already gone).
        for profile in ('kernel', 'release'):
            registered = self.full_registration(profile)
            pinned = self.pinned_probe(
                ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS[profile][1])
            with self.subTest(profile=profile, state='the full registration, every '
                                                     'pinned row skipping'):
                code, summary, _, _ = self.absent_run(profile, registered, pinned)
                self.assertEqual(code, 0, summary['failures'])
                self.assertEqual(summary['minTests'], ABSENT_FLOOR[profile])
                self.assertEqual(summary['ctestRows'], ABSENT_FLOOR[profile])
            with self.subTest(profile=profile, state='one public row left the registry'):
                code, summary, _, _ = self.absent_run(profile, registered - 1, pinned)
                self.assertEqual(code, 1)
                self.assertIn('ctest-floor', failure_stages(summary))
                self.assertEqual(summary['minTests'], ABSENT_FLOOR[profile])
                self.assertEqual(summary['ctestRows'], ABSENT_FLOOR[profile] - 1)

    def test_red_one_public_row_registered_but_not_run_fails_the_absent_floor(self):
        # Control (b): an unpinned row that stops running is a loss with no
        # margin to absorb it, even though CTest's own count still holds it.
        for profile in ('kernel', 'release'):
            pinned = self.pinned_probe(
                ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS[profile][1])
            registered = self.full_registration(profile)
            with self.subTest(profile=profile):
                code, summary, _, _ = self.absent_run(
                    profile, registered, pinned, extra_skips=['test_one_public_row_lost'])
                self.assertEqual(code, 1)
                self.assertIn('ctest-floor', failure_stages(summary))
                self.assertEqual(summary['ctestRows'], ABSENT_FLOOR[profile] - 1)
                self.assertEqual(summary['minTests'], ABSENT_FLOOR[profile])
                error = next(item['error'] for item in summary['failures']
                             if item['stage'] == 'ctest-floor')
                self.assertIn('test_one_public_row_lost', error)
                self.assertIn('below the floor', error)

    def test_green_all_pinned_rows_skip_and_everything_else_runs_passes(self):
        # Control (c): the profile's real overlay-present registration, in the
        # absent world, with exactly its pinned rows skipping -- the run the
        # campaign's absent jobs are, and it passes AT the floor.
        for profile in ('kernel', 'release'):
            registered = self.full_registration(profile)
            pinned = self.pinned_probe(
                ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS[profile][1])
            with self.subTest(profile=profile):
                code, summary, _, build_dir = self.absent_run(profile, registered, pinned)
                self.assertEqual(code, 0, summary['failures'])
                self.assertEqual(summary['minTests'], ABSENT_FLOOR[profile])
                self.assertEqual(summary['ctestRows'], ABSENT_FLOOR[profile])
                log = (build_dir / 'ci-logs' / 'ctest-floor.log').read_text()
                self.assertIn(f'lowered by {len(pinned)} pinned private-fixture '
                              f'placeholder rows (full floor {FULL_FLOOR[profile]})', log)

    def test_the_heads_real_absent_shapes_at_the_fixed_rule(self):
        # Two absent-world shapes driven through the real Driver. The dated
        # kernel absent run (rj-20261012t003154-1c9988) registered 337 rows
        # with its 28 pinned placeholders skipping: 309 ran. The current
        # kernel shape has 31 pinned skips: 337 - 31 = 306 ran, at the floor.
        # The R2 review's shrunken registry registered 900 with 298 pinned skips:
        # 602 ran, still below the current 603 (910 - 307) floor, so it FAILS --
        # the two rows the twin guards drop (R2-F2) are not bought back by a
        # registry that shrank with them. That is the RED the old observed-skip
        # floor could not show (it passed 603 against 602).
        code, summary, _, _ = self.absent_run('kernel', 337, self.pinned_probe(31))
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['minTests'], ABSENT_FLOOR['kernel'])
        self.assertEqual(summary['ctestRows'], ABSENT_FLOOR['kernel'])
        code, summary, _, _ = self.absent_run('release', 900, self.pinned_probe(298))
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertEqual(summary['minTests'], ABSENT_FLOOR['release'])
        self.assertEqual(summary['ctestRows'], 602)

    def test_min_tests_values_are_never_lowered(self):
        # R2-F8: the lowering keys on the PROFILE, not on the floor's value, so
        # an explicit --min-tests 325 is used verbatim in the absent world --
        # where the profile's own floor is 306 -- and fails a 306-row run the
        # default accepts. The replaced code lowered an explicit 325 by the
        # observed skips (204) and passed it.
        pinned = self.pinned_probe(31)
        code, summary, _, _ = self.absent_run('kernel', 337, pinned)
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['minTests'], ABSENT_FLOOR['kernel'])
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'kernel', extra=['--min-tests', '325'], ctest_registered=337,
                ctest_rows=337, ctest_skipped=pinned)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertEqual(summary['minTests'], 325)
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'kernel', extra=['--min-tests', '306'], ctest_registered=337,
                ctest_rows=337, ctest_skipped=pinned)
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['minTests'], 306)

    def test_the_lowered_note_prints_only_for_the_tables_own_lowering(self):
        # N5: an explicit --min-tests that happens to equal full minus pinned
        # was not lowered by the table; the note must not claim it was. The
        # default floor keeps printing it.
        pinned = self.pinned_probe(
            ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['kernel'][1])
        with self.overlay_absent():
            code, summary, _, build_dir = self.run_profile(
                'kernel', extra=['--min-tests', str(ABSENT_FLOOR['kernel'])],
                ctest_registered=FULL_FLOOR['kernel'], ctest_rows=FULL_FLOOR['kernel'],
                ctest_skipped=pinned)
        self.assertEqual(code, 0, summary['failures'])
        log = (build_dir / 'ci-logs' / 'ctest-floor.log').read_text()
        self.assertNotIn('lowered by', log)
        with self.overlay_absent():
            code, summary, _, build_dir = self.run_profile(
                'kernel', ctest_registered=FULL_FLOOR['kernel'],
                ctest_rows=FULL_FLOOR['kernel'], ctest_skipped=pinned)
        self.assertEqual(code, 0, summary['failures'])
        log = (build_dir / 'ci-logs' / 'ctest-floor.log').read_text()
        self.assertIn('lowered by', log)

    def test_the_old_controls_shape_registered_at_the_floor_still_behaves(self):
        # Control (e): the shape the replaced controls drove -- a run that
        # registers exactly the floor -- behaves the same way under the
        # constant: at the floor it passes, one row under it fails closed.
        for profile in ('kernel', 'release'):
            with self.subTest(profile=profile, state='at the floor'):
                code, summary, _, _ = self.absent_run(
                    profile, ABSENT_FLOOR[profile], ())
                self.assertEqual(code, 0, summary['failures'])
                self.assertEqual(summary['minTests'], ABSENT_FLOOR[profile])
            with self.subTest(profile=profile, state='one under'):
                code, summary, _, _ = self.absent_run(
                    profile, ABSENT_FLOOR[profile], ('test_quietly_skipping',))
                self.assertEqual(code, 1)
                self.assertIn('ctest-floor', failure_stages(summary))
                self.assertEqual(summary['ctestRows'], ABSENT_FLOOR[profile] - 1)

    def test_red_overlay_present_any_skipped_row_fails_the_run(self):
        # Control (d): with the overlay present the profile's whole population
        # must run, so ANY row that skips is a lost row -- a pinned one on the
        # label rule and on the floor, an unpinned one on the floor alone.
        for skipped, stages in (
                (['test_aapl15_margin_brackets_l4a'],
                 ['ctest-private-fixtures', 'ctest-floor']),
                (['test_native_live_websocket'], ['ctest-floor'])):
            with self.subTest(skipped=skipped):
                with self.overlay_present():
                    code, summary, _, _ = self.run_profile(
                        'kernel', ctest_rows=FULL_FLOOR['kernel'],
                        ctest_skipped=skipped)
                self.assertEqual(code, 1)
                self.assertEqual(failure_stages(summary), stages)
                self.assertEqual(summary['ctestRows'], FULL_FLOOR['kernel'] - 1)

    def test_green_overlay_absent_the_profiles_own_population_passes(self):
        # GREEN (flag unset): the profile's real overlay-present registration,
        # in the absent world, with exactly its pinned rows skipping, meets the
        # absent floor exactly.
        pinned = self.pinned_probe(ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['release'][1])
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'release', ctest_registered=FULL_FLOOR['release'],
                ctest_rows=FULL_FLOOR['release'], ctest_skipped=pinned)
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['ctestRows'], ABSENT_FLOOR['release'])
        self.assertEqual(summary['minTests'], ABSENT_FLOOR['release'])

    def test_red_overlay_absent_one_row_below_the_floor_fails(self):
        # RED (flag unset): one row under the constant floor fails closed, even
        # though the run registered one row fewer -- the floor does not follow
        # the registry down.
        pinned = self.pinned_probe(ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['release'][1])
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'release', ctest_registered=FULL_FLOOR['release'] - 1,
                ctest_rows=FULL_FLOOR['release'] - 1, ctest_skipped=pinned)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertEqual(summary['minTests'], ABSENT_FLOOR['release'])
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'ctest-floor')
        self.assertIn(str(ABSENT_FLOOR['release']), error)

    def test_red_absent_a_pinned_row_that_runs_fails_the_floor(self):
        # N4: a pinned row that RAN without its fixtures is counted by the
        # floor like any public row, so it hides the loss of one of them.
        # Every registered pinned row must skip in the absent world.
        pinned = self.pinned_probe(
            ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['kernel'][1])
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'kernel', ctest_registered=FULL_FLOOR['kernel'],
                ctest_rows=FULL_FLOOR['kernel'], ctest_skipped=pinned[:-1],
                ctest_registered_names=pinned)
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'ctest-floor')
        self.assertIn('pinned rows that ran: 1', error)
        self.assertIn(pinned[-1], error)
        # 337 - 30 = 307 ran, one ABOVE the 306 floor: only this rule sees it.
        self.assertEqual(summary['ctestRows'], ABSENT_FLOOR['kernel'] + 1)

    def test_green_absent_every_registered_pinned_row_skips(self):
        # N4 GREEN: the same registry with every pinned row skipping passes.
        pinned = self.pinned_probe(
            ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['kernel'][1])
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'kernel', ctest_registered=FULL_FLOOR['kernel'],
                ctest_rows=FULL_FLOOR['kernel'], ctest_skipped=pinned,
                ctest_registered_names=pinned)
        self.assertEqual(code, 0, summary['failures'])

    def test_red_absent_a_listing_without_row_names_fails_closed(self):
        # The rule reads the run's own registry: a listing whose count parses
        # but which carries no names cannot answer, and must fail closed.
        pinned = self.pinned_probe(
            ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['kernel'][1])
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'kernel', ctest_registered=FULL_FLOOR['kernel'],
                ctest_rows=FULL_FLOOR['kernel'], ctest_skipped=pinned,
                **{'ctest-list-all': 'names-less'})
        self.assertEqual(code, 1)
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'ctest-floor')
        self.assertIn('carried no row names', error)

    def test_green_absent_the_named_websocket_skip_does_not_trip_the_floor(self):
        # N6: main's kernel floor counted test_native_live_websocket's 77 as a
        # run (its number sat one below the registered count). The table's full
        # floor counts that row as running, so the one named row keeps the
        # allowance: 337 registered, 31 pinned + 1 websocket skipping, 305 ran.
        pinned = self.pinned_probe(
            ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['kernel'][1])
        with self.overlay_absent():
            code, summary, _, build_dir = self.run_profile(
                'kernel', ctest_registered=FULL_FLOOR['kernel'],
                ctest_rows=FULL_FLOOR['kernel'],
                ctest_skipped=pinned + [ci_verify.WEBSOCKET_SKIP_ROW])
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['ctestRows'], ABSENT_FLOOR['kernel'] - 1)
        log = (build_dir / 'ci-logs' / 'ctest-floor.log').read_text()
        self.assertIn('allowed as on main', log)
        self.assertIn(ci_verify.WEBSOCKET_SKIP_ROW, log)

    def test_red_absent_any_other_unpinned_skip_still_fails(self):
        # N6: the allowance is exactly ONE named row; any other unpinned skip
        # is still a loss with no margin to absorb it.
        pinned = self.pinned_probe(
            ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['kernel'][1])
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'kernel', ctest_registered=FULL_FLOOR['kernel'],
                ctest_rows=FULL_FLOOR['kernel'],
                ctest_skipped=pinned + ['test_another_unpinned_row'])
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'ctest-floor')
        self.assertIn('below the floor', error)

    def test_red_overlay_absent_an_unpinned_skip_is_a_loss(self):
        # RED (flag unset): a row that skips without being pinned is a loss --
        # there is no margin to absorb it, because the floor is a constant.
        pinned = self.pinned_probe(ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['release'][1])
        with self.overlay_absent():
            code, summary, _, _ = self.run_profile(
                'release', ctest_registered=FULL_FLOOR['release'],
                ctest_rows=FULL_FLOOR['release'],
                ctest_skipped=pinned + ['test_quietly_skipping'])
        self.assertEqual(code, 1)
        self.assertIn('ctest-floor', failure_stages(summary))
        self.assertEqual(summary['ctestRows'], ABSENT_FLOOR['release'] - 1)
        self.assertEqual(summary['minTests'], ABSENT_FLOOR['release'])


class PrExclusionWithFixturesAbsent(unittest.TestCase):
    """N1: the --exclude-label branch in the public PR jobs (fixtures absent).

    The PR Release, Debug, kernel and sanitizers jobs run `ctest -LE slow` on a tree without
    the private fixtures: the pinned placeholders are still selected and
    SKIP, and the old rule (ran == selected) failed on every one of them --
    D2's probe of the real Driver (debug, 882 registered, 5 slow, 290 pinned
    skipping) exited 1 with 'CTest registered 882, labelled 5, selected 877,
    but ran 587'. The branch now accepts exactly the pinned skips: every
    skipped row must be pinned and ran + pinned skips must equal selected,
    so an unpinned skip and a lost public row both still fail.
    """

    def run_profile(self, profile='debug', extra=None, **exits):
        exits.setdefault('source-guard-native-versions', 0)
        return DriverOrderingAndAggregation.run_profile(
            self, profile, extra=['--exclude-label', 'slow'] + list(extra or []), **exits)

    def pinned_probe(self, count: int) -> list:
        return sorted(ci_verify.private_fixture_rows(ROOT))[:count]

    def test_release_and_kernel_exclusions_hold_registration_selection_and_pinned_skips(self):
        # D1 floors are independent policy literals, not read back from the
        # implementation being tested. Exercise the real enforcement branch
        # without repeating an engine configure/build or scripted source guard.
        for profile, floor in (('release', 895), ('kernel', 337)):
            pinned = self.pinned_probe(1)
            cases = (
                (floor, floor - 5, floor - 5, pinned, 'passed', ''),
                (floor - 1, floor - 6, floor - 6, pinned, 'failed', 'registration floor'),
                (floor, floor, floor, pinned, 'failed', 'expected a nonempty'),
                (floor, 0, 0, (), 'failed', ''),
                (floor, floor - 5, floor - 6, pinned, 'failed', 'ran + pinned skips'),
                (floor, floor - 5, floor - 5, ['unapproved_skip'],
                 'failed', 'skipped but not pinned'),
            )
            for registered, selected, total, skipped, status, message in cases:
                with self.subTest(profile=profile, registered=registered, selected=selected,
                                  total=total, skipped=skipped), \
                        tempfile.TemporaryDirectory() as temporary, \
                        mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': ''}):
                    cfg = ci_verify.build_config(
                        [profile, '--exclude-label', 'slow', '--build-dir', temporary],
                        stream_output=False)
                    driver = ci_verify.Driver(cfg)
                    driver.enforce_test_floor(
                        Completed(0, ctest_output(total, skipped)),
                        registered=registered, selected=selected)
                    stage = next(item for item in driver.stages if item['name'] == 'ctest-exclusion')
                    self.assertEqual(stage['status'], status, stage)
                    if message:
                        self.assertIn(message, stage['error'])
                    self.assertNotIn('ctest-floor', [item['name'] for item in driver.stages])

    def test_green_the_d2_probe_shape_passes(self):
        # Control (a): the PR shape's own numbers. RED before the fix: exit 1,
        # 'but ran 587; not counted: 290 skipped'.
        code, summary, _, build_dir = self.run_profile(
            'debug', ctest_registered=882, ctest_labelled=5, ctest_rows=877,
            ctest_skipped=self.pinned_probe(290))
        self.assertEqual(code, 0, summary['failures'])
        self.assertIn('ctest-exclusion', stage_names(summary))
        self.assertEqual(summary['ctestRows'], 877 - 290)
        log = (build_dir / 'ci-logs' / 'ctest-exclusion.log').read_text()
        self.assertIn('587', log)
        self.assertIn('290 pinned private-fixture rows skipping', log)

    def test_red_an_unpinned_skip_fails(self):
        # Control (b): one skipped row outside the pin is a failure even
        # though the arithmetic alone would tolerate it.
        code, summary, _, _ = self.run_profile(
            'debug', ctest_registered=882, ctest_labelled=5, ctest_rows=877,
            ctest_skipped=self.pinned_probe(290) + ['test_row_not_pinned'])
        self.assertEqual(code, 1)
        self.assertIn('ctest-exclusion', failure_stages(summary))
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'ctest-exclusion')
        self.assertIn('test_row_not_pinned', error)
        self.assertIn('skipped but not pinned', error)

    def test_red_a_lost_public_row_fails(self):
        # Control (c): one public row fewer ran than the selection holds:
        # ran + pinned skips < selected.
        code, summary, _, _ = self.run_profile(
            'debug', ctest_registered=882, ctest_labelled=5, ctest_rows=876,
            ctest_skipped=self.pinned_probe(290))
        self.assertEqual(code, 1)
        self.assertIn('ctest-exclusion', failure_stages(summary))
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'ctest-exclusion')
        self.assertIn('586', error)
        self.assertIn('selected 877', error)

    def test_red_overlay_present_a_pinned_skip_fails_the_exclusion(self):
        # Control (d): with the overlay present the old rule stands -- every
        # selected row must run -- so even a pinned row that skips fails, on
        # the private-fixtures rule and on the exclusion stage.
        with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'}):
            code, summary, _, _ = self.run_profile(
                'debug', ctest_registered=882, ctest_labelled=5, ctest_rows=877,
                ctest_skipped=self.pinned_probe(1))
        self.assertEqual(code, 1)
        self.assertIn('ctest-exclusion', failure_stages(summary))
        self.assertIn('ctest-private-fixtures', failure_stages(summary))

    def test_red_overlay_present_an_unpinned_skip_fails_the_exclusion(self):
        # Control (d) again: an unpinned skip is refused too while the
        # overlay is present (only the exclusion stage catches it).
        with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'}):
            code, summary, _, _ = self.run_profile(
                'debug', ctest_registered=882, ctest_labelled=5, ctest_rows=877,
                ctest_skipped=['test_native_live_websocket'])
        self.assertEqual(code, 1)
        self.assertEqual(failure_stages(summary), ['ctest-exclusion'])

    def test_green_the_floor_path_is_unchanged_without_the_flag(self):
        # Control (e): the release shape (no --exclude-label) never enters the
        # exclusion stage; the constant floor still governs it.
        pinned = self.pinned_probe(ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['release'][1])
        code, summary, _, _ = DriverOrderingAndAggregation.run_profile(
            self, 'release', ctest_registered=ABSENT_FLOOR['release'],
            ctest_rows=FULL_FLOOR['release'], ctest_skipped=pinned)
        self.assertEqual(code, 0, summary['failures'])
        self.assertIn('ctest-floor', stage_names(summary))
        self.assertNotIn('ctest-exclusion', stage_names(summary))

    def test_red_an_unreadable_pin_fails_the_exclusion_closed(self):
        # The pin file speaks only while it carries PRIVATE_FIXTURE_ROW_COUNT
        # names: a truncated pin cannot clear the skips it lost.
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / 'tests').mkdir()
            (source / 'VERSION').write_text('0.0.0\n')
            rows = (ROOT / 'tests' / 'private_fixture_rows.txt').read_text().splitlines()
            (source / 'tests' / 'private_fixture_rows.txt').write_text(
                '\n'.join(rows[:len(rows) // 2]) + '\n')
            cfg = ci_verify.build_config(
                ['debug', '--exclude-label', 'slow', '--build-dir', str(source / 'build')],
                source=source, runner=default_runner, stream_output=False)
            driver = ci_verify.Driver(cfg)
            driver.enforce_test_floor(
                Completed(0, ctest_output(877, tuple(self.pinned_probe(2))), b''),
                registered=882, selected=877)
        self.assertEqual([stage['status'] for stage in driver.stages], ['failed'])
        self.assertIn('a truncated pin cannot clear the rows it lost',
                      driver.stages[0]['error'])
        self.assertEqual(driver.stages[0]['name'], 'ctest-exclusion')


class LiveProfilesWithFixturesAbsent(unittest.TestCase):
    """X2: the required native-live job on a tree without the private fixtures.

    live-sanitizers and live-tsan run ONLY the runner/ rows and refuse every
    skip; without the overlay runner/ registers its pinned private-fixture
    placeholders and they SKIP, so the old rule failed the required
    native-live job of every public PR (.github/workflows/ci.yml's build
    gate). The branch now accepts exactly the pinned skips -- every skipped
    row must be a pinned private-fixture row -- while an unpinned skip, a
    not-run row and a disabled row still fail, and with the overlay present
    the old rule stands: nothing may skip.
    """

    def run_profile(self, profile='live-sanitizers', extra=None, **exits):
        # The real source guards run in this harness, and this tree has no
        # private fixtures: check_native_cpp_versions genuinely exits 77 in
        # BOTH worlds, accepted while the overlay is absent and refused under
        # it (R2-F3). These controls are about the runner-skip rule, so that
        # one guard is scripted as the world leaves it, as the sibling
        # control classes do.
        exits.setdefault('source-guard-native-versions', 0)
        return DriverOrderingAndAggregation.run_profile(
            self, profile, extra=extra, **exits)

    def pinned_probe(self, count: int) -> list:
        # Required e2e rows must execute even when other pinned rows skip.
        pinned = sorted(ci_verify.private_fixture_rows(ROOT)
                        - ci_verify.runner_e2e_test_names(ROOT))
        self.assertGreaterEqual(len(pinned), count)
        return pinned[:count]

    def test_green_the_absent_runner_placeholders_skip_and_the_profile_passes(self):
        # Control (a): the public native-live shape. The runner/ tree
        # registers its 65 old pinned placeholders beside the required rows;
        # they skip, ran stays at the floor, and the profile passes with the
        # acceptance printed.
        pinned = self.pinned_probe(65)
        code, summary, _, build_dir = self.run_profile(
            'live-sanitizers', ctest_registered=65 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_rows=65 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_skipped=pinned)
        self.assertEqual(code, 0, summary['failures'])
        self.assertEqual(summary['ctestRows'], LIVE_SANITIZERS_MIN_TESTS)
        self.assertNotIn('live-test-skips', failure_stages(summary))
        self.assertIn('live-test-skips', stage_names(summary))
        log = (build_dir / 'ci-logs' / 'live-test-skips.log').read_text()
        self.assertIn('65 pinned private-fixture rows skipping', log)

    def test_red_an_unpinned_runner_skip_fails(self):
        # Control (b): one skipped row outside the pin is a failure, even
        # though the arithmetic alone would tolerate it.
        pinned = self.pinned_probe(65)
        code, summary, _, _ = self.run_profile(
            'live-sanitizers', ctest_registered=66 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_rows=66 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_skipped=pinned + ['test_unpinned_runner_row'])
        self.assertEqual(code, 1)
        self.assertIn('live-test-skips', failure_stages(summary))
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'live-test-skips')
        self.assertIn('test_unpinned_runner_row', error)
        self.assertIn('not pinned private-fixture placeholders', error)

    def test_red_a_not_run_row_fails_with_the_pinned_skips(self):
        # Control (c): a row CTest could not start is refused beside the
        # pinned skips -- the acceptance covers skips, nothing else.
        pinned = self.pinned_probe(65)
        code, summary, _, _ = self.run_profile(
            'live-sanitizers', ctest_registered=66 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_rows=66 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_skipped=pinned, ctest_not_run=['test_live_not_run_row'])
        self.assertEqual(code, 1)
        self.assertIn('live-test-skips', failure_stages(summary))
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'live-test-skips')
        self.assertIn('cannot accept skipped, disabled or unreadable rows', error)

    def test_red_a_disabled_row_fails_with_the_pinned_skips(self):
        # Control (c) again, for the disabled list: CTest leaves it out of its
        # own count, so only this rule can refuse it.
        pinned = self.pinned_probe(65)
        code, summary, _, _ = self.run_profile(
            'live-sanitizers', ctest_registered=65 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_rows=65 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_skipped=pinned, ctest_disabled=['test_live_disabled_row'])
        self.assertEqual(code, 1)
        self.assertIn('live-test-skips', failure_stages(summary))

    def test_red_overlay_present_a_pinned_skip_still_fails(self):
        # Control (d): with the overlay present the old rule stands -- every
        # runner row must run -- so a pinned row that skips fails on the
        # private-fixtures rule and on this one.
        with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': '1'}):
            code, summary, _, _ = self.run_profile(
                'live-sanitizers', ctest_registered=LIVE_SANITIZERS_MIN_TESTS + 1,
                ctest_rows=LIVE_SANITIZERS_MIN_TESTS + 1,
                ctest_skipped=self.pinned_probe(1))
        self.assertEqual(code, 1)
        self.assertIn('live-test-skips', failure_stages(summary))
        self.assertIn('ctest-private-fixtures', failure_stages(summary))
        error = next(item['error'] for item in summary['failures']
                     if item['stage'] == 'live-test-skips')
        self.assertIn('cannot accept skipped', error)

    def test_green_live_tsan_carries_the_same_rule(self):
        # The rule keys on the profile SET: live-tsan accepts the pinned
        # placeholders the same way.
        pinned = self.pinned_probe(65)
        code, summary, _, _ = self.run_profile(
            'live-tsan', ctest_registered=65 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_rows=65 + LIVE_SANITIZERS_MIN_TESTS,
            ctest_skipped=pinned)
        self.assertEqual(code, 0, summary['failures'])
        self.assertIn('live-test-skips', stage_names(summary))

    def test_required_equivalence_skip_fails_even_when_other_rows_cover_the_floor(self):
        # Inventory membership and a healthy total cannot substitute for this
        # required driver's execution, even though its skip is a pinned one.
        for profile in ci_verify.LIVE_ONLY_PROFILES:
            with self.subTest(profile=profile):
                code, summary, _, _ = self.run_profile(
                    profile, ctest_registered=LIVE_SANITIZERS_MIN_TESTS + 1,
                    ctest_rows=LIVE_SANITIZERS_MIN_TESTS + 1,
                    ctest_skipped=['native_live_equivalence_e2e'])
                self.assertEqual(code, 1)
                self.assertEqual(summary['ctestRows'], LIVE_SANITIZERS_MIN_TESTS)
                self.assertIn('live-test-skips', failure_stages(summary))
                error = next(item['error'] for item in summary['failures']
                             if item['stage'] == 'live-test-skips')
                self.assertIn('required runner e2e rows skipped', error)
                self.assertIn('native_live_equivalence_e2e', error)

    def test_the_native_profile_is_not_under_this_rule(self):
        # Control (e): the native profile runs runner rows too but is not a
        # live-only profile; its shape is unchanged -- the runner-skip rule
        # adds no stage there (and with the overlay absent its own rules are
        # the floor's, which this profile declares none of).
        pinned = self.pinned_probe(1)
        code, summary, _, _ = self.run_profile(
            'native', ctest_registered=963, ctest_rows=963,
            ctest_skipped=pinned)
        self.assertEqual(code, 0, summary['failures'])
        self.assertNotIn('live-test-skips', stage_names(summary))
        self.assertNotIn('ctest-floor', stage_names(summary))


class FloorDocsPointAtTheTable(unittest.TestCase):
    """N3: the floors have ONE source, and the prose says where it is.

    KERNEL_MIN_TESTS / RELEASE_MIN_TESTS were a dead second copy: the
    --min-tests help quoted 325 and 895 (false), and six doc places told
    contributors to raise the constants. The two names are now plain
    module-level integer literals that the table references (the campaign's
    proof kit reads each one from the tree as a single line), and this
    control fails when the help text or a doc place goes back to a number of
    its own, or when the literals and the table move apart.
    """

    STALE_INSTRUCTIONS = (
        'raise the floor in the same commit',
        'Raise the constant when a row lands',
        'Adding rows means raising the floor',
        'raise the floor',
    )
    DOC_PLACES = ('CONTRIBUTING.md', 'docs/pages/contributing-llm.md',
                  'docs/pages/native-engine.md', 'docs/ci.md',
                  '.github/workflows/ci.yml')

    def test_the_two_names_are_literals_the_table_references(self):
        # AST teeth, in the direction the tree now fixes: the right-hand sides
        # must be plain integer literals (CORE's proof kit reads each from the
        # tree as one line), and the table's `full` entries must reference the
        # NAMES -- writing a number into the table, or deriving a name from it,
        # fails here.
        module = ast.parse((ROOT / 'scripts' / 'ci_verify.py').read_text())
        assigned, table = {}, None
        for node in module.body:
            if isinstance(node, ast.Assign) and len(node.targets) == 1:
                target = node.targets[0]
                if not isinstance(target, ast.Name):
                    continue
                if target.id in ('KERNEL_MIN_TESTS', 'RELEASE_MIN_TESTS'):
                    assigned[target.id] = node.value
                elif target.id == 'PRIVATE_FIXTURE_PROFILE_FLOORS':
                    table = node.value
        self.assertEqual(set(assigned), {'KERNEL_MIN_TESTS', 'RELEASE_MIN_TESTS'})
        for name, value in assigned.items():
            with self.subTest(name=name):
                self.assertIsInstance(value, ast.Constant)
                self.assertIsInstance(value.value, int)
        self.assertIsInstance(table, ast.Dict)
        entries = {}
        for key, value in zip(table.keys, table.values):
            self.assertIsInstance(key, ast.Constant)
            self.assertIsInstance(value, ast.Tuple)
            entries[key.value] = value.elts[0]
        self.assertEqual(set(entries), {'release', 'kernel'})
        self.assertEqual(sorted(node.id for node in entries.values()),
                         ['KERNEL_MIN_TESTS', 'RELEASE_MIN_TESTS'])
        self.assertEqual(ci_verify.KERNEL_MIN_TESTS,
                         ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['kernel'][0])
        self.assertEqual(ci_verify.RELEASE_MIN_TESTS,
                         ci_verify.PRIVATE_FIXTURE_PROFILE_FLOORS['release'][0])

    def test_the_help_text_names_the_table_and_no_stale_number(self):
        result = subprocess.run(
            [sys.executable, '-B', str(ROOT / 'scripts' / 'ci_verify.py'), '--help'],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('PRIVATE_FIXTURE_PROFILE_FLOORS', result.stdout)
        self.assertNotIn('defaults to 325', result.stdout)
        self.assertNotIn('895', result.stdout)

    def test_every_doc_place_points_at_the_table(self):
        for rel in self.DOC_PLACES:
            text = (ROOT / rel).read_text()
            with self.subTest(place=rel):
                self.assertIn('PRIVATE_FIXTURE_PROFILE_FLOORS', text)
                for stale in self.STALE_INSTRUCTIONS:
                    self.assertNotIn(stale, text)
        for rel in ('docs/pages/contributing-llm.md', 'docs/pages/native-engine.md',
                    'CONTRIBUTING.md', '.github/workflows/ci.yml'):
            with self.subTest(place=rel):
                text = (ROOT / rel).read_text()
                self.assertNotIn('KERNEL_MIN_TESTS', text)
                self.assertNotIn('RELEASE_MIN_TESTS', text)


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
    def collect(self, build: Path, output: Path, profile='release') -> None:
        env = os.environ.copy()
        env.pop('GITHUB_STEP_SUMMARY', None)
        result = subprocess.run(
            [sys.executable, str(ROOT / 'scripts/collect_ci_diagnostics.py'),
             '--build-dir', str(build), '--profile', profile, '--output', str(output)],
            capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_live_sanitizers_retain_runner_last_test_log_separately(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build'
            logs = {'Testing/Temporary/LastTest.log': 'engine CTest output\n',
                    'runner/Testing/Temporary/LastTest.log': 'runner ASan/UBSan output\n'}
            for relative, content in logs.items():
                path = build / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            output = root / 'diagnostics'
            self.collect(build, output, profile='live-sanitizers')
            self.assertEqual((output / 'LastTest.log').read_text(),
                             logs['Testing/Temporary/LastTest.log'])
            self.assertEqual((output / 'runner-LastTest.log').read_text(),
                             logs['runner/Testing/Temporary/LastTest.log'])
            missing = json.loads((output / 'missing.json').read_text())
            self.assertFalse(set(logs) & set(missing))

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


class CTestDeadlines(unittest.TestCase):
    def test_no_stage_deadline_never_refuses_a_row_however_slow(self):
        # Dispatch has no stage deadline or row-vs-stage inventory restriction.
        for flags in ([], ['--exclude-label', 'slow']):
            with self.subTest(flags=flags):
                runner = mock.Mock(return_value=Completed(0, b'row finished\n'))
                cfg = ci_verify.build_config(['native', *flags], runner=runner,
                                             stream_output=False)
                with tempfile.TemporaryDirectory() as temporary:
                    cfg.build_dir = Path(temporary)
                    driver = ci_verify.Driver(cfg)
                    argv = ci_verify.ctest_command(cfg, cfg.build_dir, jobs=1, junit=False)
                    result = driver.invoke('ctest', argv)
                self.assertEqual(result.returncode, 0)
                runner.assert_called_once_with(argv, extra_env=None,
                                               combine_stderr=True, stream_output=False)
                self.assertEqual([stage['name'] for stage in driver.stages], ['ctest'])

    def test_subprocess_dispatch_has_no_deadline_and_keeps_exit_status(self):
        for code in (0, 8, -9):
            with self.subTest(code=code), mock.patch('ci_verify.subprocess.run') as run:
                run.return_value = subprocess.CompletedProcess(['ctest'], code,
                                                              b'row output', b'diagnostic')
                result = ci_verify.call_runner(default_runner, ['ctest'],
                                              combine_stderr=False, stream_output=False)
                run.assert_called_once()
                self.assertNotIn('timeout', run.call_args.kwargs)
                self.assertEqual(result, Completed(code, b'row output', b'diagnostic'))

    def test_commands_keep_every_profile_inventory_and_only_explicit_label_exclusion(self):
        for profile in ('release', 'debug', 'kernel', 'sanitizers',
                        'native', 'live-sanitizers', 'live-tsan'):
            cfg = ci_verify.build_config([profile])
            argv = ci_verify.ctest_command(cfg, cfg.build_dir, jobs=16, junit=True)
            self.assertNotIn('-R', argv)
            self.assertNotIn('-E', argv)
            self.assertNotIn('-LE', argv)
            self.assertIn('--no-tests=error', argv)
            self.assertEqual(argv[argv.index('--timeout') + 1], '0')
        cfg = ci_verify.build_config(['native', '--exclude-label', 'slow'])
        argv = ci_verify.ctest_command(cfg, cfg.build_dir, jobs=16, junit=False)
        self.assertEqual(argv[argv.index('-LE') + 1], 'slow')
        self.assertEqual(argv[argv.index('--timeout') + 1], '0')

    def test_profiles_have_no_deadline_config_or_summary_fields(self):
        for profile in ('release', 'debug', 'kernel', 'native', 'sanitizers',
                        'live-sanitizers', 'live-tsan'):
            with self.subTest(profile=profile):
                cfg = ci_verify.build_config([profile])
                self.assertFalse(hasattr(cfg, 'test_timeout'))
                self.assertFalse(hasattr(cfg, 'ctest_stage_timeout'))
                summary = ci_verify.Driver(cfg).summary
                self.assertNotIn('testTimeoutSeconds', summary)
                self.assertNotIn('ctestTimeoutSeconds', summary)
                argv = ci_verify.ctest_command(cfg, cfg.build_dir, jobs=1, junit=False)
                self.assertEqual(argv[argv.index('--timeout') + 1], '0')
        # An exclusion label does not introduce a deadline.
        cfg = ci_verify.build_config(['sanitizers', '--exclude-label', 'slow'])
        argv = ci_verify.ctest_command(cfg, cfg.build_dir, jobs=1, junit=False)
        self.assertEqual(argv[argv.index('--timeout') + 1], '0')

    def test_removed_deadline_options_are_unrecognised(self):
        for flags in (['--test-timeout', '0'], ['--test-timeout', '-1'],
                      ['--ctest-timeout', '0'], ['--ctest-timeout', '-1'],
                      ['--test-timeout', '1800'], ['--ctest-timeout', '2400'],
                      ['--test-timeout', '900', '--ctest-timeout', '900'],
                      ['--test-timeout', '2401', '--ctest-timeout', '2400']):
            with self.subTest(flags=flags), self.assertRaisesRegex(ConfigError, 'unrecognized arguments'):
                ci_verify.build_config(['native', *flags])

    def test_real_ctest_names_the_hang_and_runs_the_remaining_row(self):
        # A deliberate fixture TIMEOUT must still fail with exit 8, name its
        # row, run the witness and retain supported JUnit. No verifier deadline.
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / 'CTestTestfile.cmake').write_text(
                f'add_test(hanging "{shutil.which("cmake")}" -E sleep 20)\n'
                'set_tests_properties(hanging PROPERTIES TIMEOUT 1)\n'
                f'add_test(witness "{shutil.which("cmake")}" -E echo executed)\n')
            cfg = ci_verify.build_config(['native', '--build-dir', str(build),
                                          '--min-tests', '2'])
            driver = ci_verify.Driver(cfg)
            junit = ci_verify.ctest_supports_junit(ci_verify.default_runner)
            argv = ci_verify.ctest_command(cfg, build, jobs=1, junit=junit)
            result = driver.invoke('ctest', argv, stream_output=False)
            driver.enforce_test_floor(result, registered=2, selected=2)
            self.assertEqual(result.returncode, 8, result.stdout)
            self.assertIn(b'hanging', result.stdout)
            self.assertIn(b'Timeout', result.stdout)
            self.assertIn(b'witness', result.stdout)
            self.assertRegex(result.stdout, rb'witness\s+\.+\s+Passed')
            self.assertEqual(driver.summary['ctestRows'], 2)
            self.assertEqual(driver.summary['ctestSkipped'], [])
            self.assertEqual(driver.stages[0]['status'], 'failed')
            self.assertEqual(driver.stages[0]['exitCode'], 8)
            self.assertIn('ctest', [failure['stage'] for failure in driver.failures])
            if junit:
                self.assertTrue((build / 'ctest-junit.xml').is_file())
                junit_text = (build / 'ctest-junit.xml').read_text()
                self.assertIn('hanging', junit_text)
                self.assertIn('witness', junit_text)
                self.assertIn('<failure', junit_text)
            self.assertIn('Timeout', (build / 'ci-logs/ctest.log').read_text())

    def test_failed_and_skipped_labelled_rows_keep_their_counts(self):
        # Newer CTest prints a listed row's labels after its status, as a main
        # run printed '\t  2 - test_ci_verify (Timeout)    slow' for the timed-out
        # slow row. Those rows must stay counted so a failed run's summary
        # still records how many rows ran, were skipped or could not start.
        self.assertEqual(ctest_rows(
            b'99% tests passed, 1 tests failed out of 941\n\nThe following tests FAILED:\n'
            b'\t  2 - test_ci_verify (Timeout)                          slow\n'
            b'Errors while running CTest\n').ran, 941)
        python = sys.executable
        inventory = '\n'.join((
            f'add_test(labelled_pass "{python}" "-c" "raise SystemExit(0)")',
            f'add_test(labelled_fail "{python}" "-c" "raise SystemExit(3)")',
            f'add_test(labelled_skip "{python}" "-c" "raise SystemExit(77)")',
            'add_test(labelled_not_run "/nonexistent/ci-verify-missing-executable")',
            'set_tests_properties(labelled_pass labelled_fail labelled_skip labelled_not_run '
            'PROPERTIES LABELS "slow;live")',
            'set_tests_properties(labelled_skip PROPERTIES SKIP_RETURN_CODE 77)',
        ))
        with tempfile.TemporaryDirectory() as temporary:
            (Path(temporary) / 'CTestTestfile.cmake').write_text(inventory + '\n')
            discovered = default_runner(
                ['ctest', '--test-dir', temporary, '--show-only=json-v1'],
                stream_output=False)
            self.assertEqual(discovered.returncode, 0, discovered.stdout)
            registered = json.loads(discovered.stdout)['tests']
            self.assertEqual({test['name'] for test in registered},
                             {'labelled_pass', 'labelled_fail', 'labelled_skip',
                              'labelled_not_run'})
            for test in registered:
                with self.subTest(name=test['name']):
                    properties = {item['name']: item['value']
                                  for item in test['properties']}
                    self.assertEqual(set(properties['LABELS']), {'slow', 'live'})
            result = default_runner(
                ['ctest', '--test-dir', temporary, '--output-on-failure', '--no-tests=error',
                 '--parallel', '2'], stream_output=False)
        output = result.stdout.decode('utf-8', 'replace')
        self.assertEqual(result.returncode, 8, output)
        self.assertRegex(output,
                         r'(?m)^[ \t]*2 - labelled_fail \(Failed\)(?:[ \t]+\S.*)?[ \t]*$',
                         output)
        rows = ctest_rows(result.stdout)
        self.assertEqual((rows.total, rows.ran), (4, 2), output)
        self.assertEqual(rows.skipped, ('labelled_skip',), output)
        self.assertEqual(rows.not_run, ('labelled_not_run',), output)


class NativeLiveEquivalenceCTest(unittest.TestCase):
    """CR-223-ROW: source wiring and compiler-free adapter controls.

    The tiny driver below is a fixture, never the corpus e2e or a compiler.
    The supervisor separately proves the registered row with the real driver.
    """

    def test_registered_row_has_build_tree_outputs_and_status_only_skip(self):
        cmake = (ROOT / 'runner' / 'CMakeLists.txt').read_text()
        row = re.search(r'add_test\(NAME native_live_equivalence_e2e\s+(.*?)\)',
                        cmake, re.S)
        self.assertIsNotNone(row, 'the equivalence driver needs a permanent runner CTest row')
        command = row.group(1)
        self.assertIn('${Python3_EXECUTABLE} -B', command)
        self.assertIn('${PROJECT_SOURCE_DIR}/tests/native_live_equivalence_ctest.py', command)
        self.assertIn('--script ${PROJECT_SOURCE_DIR}/tests/native_live_equivalence_e2e.py', command)
        self.assertIn('--build-dir ${PROJECT_BINARY_DIR}', command)
        for option in ('--libraries-dir', '--out'):
            self.assertRegex(command, re.escape(option) + r'\s+\$\{CMAKE_CURRENT_BINARY_DIR\}/')
        properties = re.search(
            r'set_tests_properties\(native_live_equivalence_e2e\s+PROPERTIES\s+(.*?)\)',
            cmake, re.S)
        self.assertIsNotNone(properties)
        self.assertRegex(properties.group(1), r'\bSKIP_RETURN_CODE\s+77\b')
        self.assertRegex(properties.group(1), r'\bLABELS\s+private_fixtures\b')
        self.assertNotRegex(properties.group(1),
                            r'\b(SKIP_REGULAR_EXPRESSION|PASS_REGULAR_EXPRESSION|WILL_FAIL|DISABLED)\b')
        self.assertIn('native_live_equivalence_e2e', ci_verify.private_fixture_rows(ROOT))
        # The same runtime preload/finalization contract must reach both
        # ctypes witnesses in ASan and TSan builds.
        self.assertRegex(cmake, r'set_tests_properties\(native_live_genuine_retry_e2e\s+'
                         r'native_live_equivalence_e2e\s+PROPERTIES ENVIRONMENT')

    def test_inventory_decision_and_prerequisites_are_documented(self):
        source = (ROOT / 'scripts' / 'ci_verify.py').read_text()
        function = source.split('def runner_e2e_test_names(', 1)[1].split('\n\n', 1)[0]
        self.assertIn('native-live.yml', function)
        self.assertIn('corpus', function)
        self.assertIn('docs/ci.md', function)
        docs = (ROOT / 'docs' / 'ci.md').read_text()
        self.assertIn('`native_live_equivalence_e2e`', docs)
        self.assertIn('runner_e2e_test_names()', docs)
        self.assertNotIn('corpus equivalence driver is not a runner CTest row', docs)

    def test_mandatory_row_raises_live_run_floor_in_both_fixture_worlds(self):
        # All twelve existing public rows and the equivalence row must RUN.
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            for transport_present in (False, True):
                if transport_present:
                    (source / 'runner').mkdir()
                    (source / 'runner' / 'transport.cpp').write_text('// fixture\n')
                for overlay in ('', '1'):
                    with self.subTest(transport=transport_present, overlay=overlay), \
                            mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': overlay}):
                        for profile in ci_verify.LIVE_ONLY_PROFILES:
                            self.assertEqual(ci_verify.profile_min_tests(ci_verify.PROFILE[profile], source),
                                             13 if transport_present else 12)

    def test_live_feed_materialization_follows_profile_and_is_lfs_explicit(self):
        # Exercise the prerequisite method alone: no guards, configure or
        # compiler is invoked by this control.
        for profile in ci_verify.PROFILES:
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as temporary:
                config = validate_config(parse_args(
                    [profile, '--build-dir', temporary], source=ROOT), source=ROOT)
                driver = ci_verify.Driver(config)
                calls = []

                def invoke(stage, argv):
                    calls.append((stage, argv))
                    return Completed(0, b' 0123456789abcdef corpus\n', b'')

                with mock.patch.object(driver, 'invoke', side_effect=invoke):
                    self.assertTrue(driver.ensure_corpus_submodule())
                lfs = [argv for stage, argv in calls if stage == 'corpus-live-feed']
                expected = config.profile.live_runner and config.profile.source_layer
                self.assertEqual(bool(lfs), expected)
                if expected:
                    self.assertEqual(lfs, [[
                        'git', '-C', str(ROOT / 'corpus'), 'lfs', 'pull',
                        '--include=data/ohlcv_ETH-USDT-USDT_1m.csv', '--exclude=']])
        workflow = (ROOT / '.github' / 'workflows' / 'native-live.yml').read_text()
        install = next(line for line in workflow.splitlines() if 'apt-get install' in line)
        self.assertIn('git-lfs', install.split())

    def fixture(self, temporary, *, missing=None, exit_code=0, exception=False):
        root = Path(temporary) / 'source'
        build = Path(temporary) / 'build'
        script = root / 'tests' / 'native_live_equivalence_e2e.py'
        script.parent.mkdir(parents=True)
        script.write_text(
            "PROBES = {'probe-alpha': {'market'}, 'probe-beta': {'limit'}}\n"
            "import argparse, json\nfrom pathlib import Path\n"
            "parser = argparse.ArgumentParser()\n"
            "for option in ('--root', '--build-dir', '--libraries-dir', '--out'):\n"
            "    parser.add_argument(option, required=True)\n"
            "args = parser.parse_args()\n"
            "Path(args.out).mkdir(parents=True, exist_ok=False)\n"
            "Path(args.libraries_dir).mkdir(parents=True, exist_ok=False)\n"
            "print('DRIVER_EXECUTED ' + json.dumps(vars(args)), flush=True)\n"
            + ("raise RuntimeError('driver fixture failure')\n" if exception
               else f"raise SystemExit({exit_code})\n"))
        paths = [
            build / 'bin' / 'pineforge-live', build / 'compile_commands.json',
            build / 'lib' / 'libpineforge.a',
            root / 'corpus' / 'data' / 'ohlcv_ETH-USDT-USDT_1m.csv',
            root / 'tests' / 'native_live_receipts.py',
            *(root / 'corpus' / 'validation' / probe / 'generated.cpp'
              for probe in ('probe-alpha', 'probe-beta')),
        ]
        for path in paths:
            if missing is None or not path.as_posix().endswith(missing):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('// prerequisite fixture only\n')
        command = [sys.executable, '-B', str(ROOT / 'tests' / 'native_live_equivalence_ctest.py'),
                   '--script', str(script), '--root', str(root), '--build-dir', str(build),
                   '--libraries-dir', str(build / 'libraries'), '--out', str(build / 'results')]
        return command

    def invoke_fixture(self, command):
        return subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env={key: value for key, value in os.environ.items()
                                   if key not in ('LD_PRELOAD', 'DYLD_INSERT_LIBRARIES')})

    def test_each_missing_prerequisite_skips_once_without_executing_driver(self):
        for missing in ('bin/pineforge-live', 'compile_commands.json', 'lib/libpineforge.a',
                        'corpus/data/ohlcv_ETH-USDT-USDT_1m.csv', 'tests/native_live_receipts.py',
                        'corpus/validation/probe-alpha/generated.cpp',
                        'corpus/validation/probe-beta/generated.cpp'):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as temporary:
                result = self.invoke_fixture(self.fixture(temporary, missing=missing))
                self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
                self.assertEqual(len(result.stdout.splitlines()), 1)
                self.assertTrue(result.stdout.startswith(
                    'SKIP native_live_equivalence_e2e prerequisites absent: '), result.stdout)
                self.assertIn(missing, result.stdout)
                self.assertNotIn('DRIVER_EXECUTED', result.stdout)

    def test_ready_driver_executes_and_runtime_failures_never_become_skips(self):
        for exit_code in (0, 1, 2, 77, 256, -256):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as temporary:
                result = self.invoke_fixture(self.fixture(temporary, exit_code=exit_code))
                self.assertIn('DRIVER_EXECUTED ', result.stdout, result.stdout + result.stderr)
                self.assertEqual(result.returncode, 0 if exit_code == 0 else 1,
                                 result.stdout + result.stderr)
                self.assertNotIn('SKIP ', result.stdout)
        with tempfile.TemporaryDirectory() as temporary:
            result = self.invoke_fixture(self.fixture(temporary, exception=True))
            self.assertNotEqual(result.returncode, 0)
            self.assertNotEqual(result.returncode, 77)
            self.assertIn('driver fixture failure', result.stderr)

    def test_reruns_retain_distinct_outputs_and_libraries_under_build_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            command = self.fixture(temporary)
            receipts = []
            for _ in range(2):
                result = self.invoke_fixture(command)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                receipt = json.loads(next(line.removeprefix('DRIVER_EXECUTED ')
                                         for line in result.stdout.splitlines()
                                         if line.startswith('DRIVER_EXECUTED ')))
                for option in ('out', 'libraries_dir'):
                    self.assertTrue(Path(receipt[option]).is_relative_to(Path(temporary) / 'build'))
                    self.assertTrue(Path(receipt[option]).is_dir())
                receipts.append(receipt)
            for option in ('out', 'libraries_dir'):
                self.assertNotEqual(receipts[0][option], receipts[1][option])
                self.assertTrue(Path(receipts[0][option]).is_dir())


if __name__ == '__main__':
    unittest.main()
