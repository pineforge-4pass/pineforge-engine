#!/usr/bin/env python3
"""Shared local/CI verification driver. Stdlib only. Not a command generator.

Profiles: release, debug, sanitizers, native, kernel. Default build dir build-ci-PROFILE.
Source guards, explicit configure, full rebuild, pinned e60/0e/v13/v14/v15/v16 ABI prepare/reuse,
CTest, install+find_package+VERSION smoke, native help / required WebSocket.
Fail fast on configure/build. After a successful build collect independent
CTest and package failures in the same run. Never deletes source, tests, or
build trees; a mismatched ABI base is a refusal to a fresh --build-dir.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Callable

from prepare_settlement_cpp_abi_base import (
    BASE_COMMIT,
    BASE_TREE,
    PROVIDERS,
    read_cache,
    reusable_prepared_base,
)

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ('release', 'debug', 'sanitizers', 'native', 'kernel')
DEFAULT_JOBS = 4
JOBS_MIN, JOBS_MAX = 1, 64
SCHEMA = 'pineforge-ci-verify/v1'
SANITIZER_FLAG = '-fsanitize=address,undefined'
# Kernel-row floor. The kernel profile registers only the source-free CTest
# rows (tests/CMakeLists.txt drops every test TU whose include closure reaches
# pineforge/source or compat/pine), so a lane whose native pin sits beside an
# adapter twin in one TU silently leaves the kernel-only gate. The floor pins
# the row count the profile is expected to run: adding source-free rows never
# trips it, losing them does, and a run that reports no count fails closed.
# 176 = the 158 rows the profile ran before lane N3, plus the six native halves
# N3 freed (L2 report truth, L4b margin hooks, L5 calc timing, L6 HTF
# subscriptions, L8/L8b price grid, L11a lot excursion) = 164, plus the ten
# example_* rows N4 brought into this profile when it turned
# PINEFORGE_BUILD_EXAMPLES on here, plus N5's test_native_host_hash_extension
# and N6's test_native_margin_fx_roll. N9 and N8 added no source-free TU of
# their own: both extended rows the profile already ran. Raise it when a new
# source-free row lands. --min-tests overrides it for any profile.
# 181 = those 176 rows plus gap wave B's five: N13's test_native_arm_options,
# N7's test_native_auxiliary_feed and test_native_auxiliary_feed_stream (its
# adapter twin is a source-layer TU and stays out), and N11's two source-guard
# rows test_adapter_spec_shadowing / _mutations, which are Python guards over
# the sources and so register in every profile. N10 and N12 added no
# source-free row: N10 re-pointed six trail TUs at a tests-only oracle header
# (they keep running here) and N12's differential is an adapter TU.
# 182 = those 181 plus N18's test_native_open_lots, plus N14's
# test_native_tail_override_seam, minus N14's test_market_admission_causality,
# which left this profile when the admission journal became source-layer state
# (it still runs in every profile that builds the source layer). N15 adds no
# CTest row: its corpus-parity gate is a ci_preflight stage and a CI job.
# 183 = those 182 plus gap lane P2's test_kernel_residuals, the Python guard
# that holds ADR-0001's residual-vocabulary table against libpineforge_kernel.a
# (a source-free row: it reads the archive every profile builds).
# 184 = those 183 plus P5's test_native_limit_fill_through (the market-if-
# touched geometry of Limit{price, fill_through}); P5's closed_trade(i) and
# liquidation-through-the-lots witnesses extend existing source-free TUs.
# 187 = those 184 plus audit lane P6's three example_* rows for the two
# features the adapter never declares: example_native_price_grid_strategy,
# example_native_price_grid_c and example_native_risk_limits_strategy.
# 189 = those 187 plus P6's two source-guard rows test_native_feature_rulings /
# _mutations, which are Python guards over the sources and so register in
# every profile, as N11's two do.
# 190 = those 189 plus lane P7's test_example_runner, the self-test of the
# runner every example_* row goes through; it registers wherever the examples
# do, so here and in release.
KERNEL_MIN_TESTS = 190
# CTest's closing summary: '100% tests passed out of N' when nothing failed,
# '97% tests passed, 3 tests failed out of N' otherwise.
CTEST_ROW_COUNT = re.compile(r'% tests passed(?:, \d+ tests? failed)? out of (\d+)')
# LeakSanitizer is unavailable in Apple's ASan runtime.  Keep the Linux CI
# lane strict, while allowing the local macOS ASan/UBSan profile to execute
# its actual instrumented tests instead of failing during runtime startup.
_ASAN_LEAKS = '0' if sys.platform == 'darwin' else '1'
SANITIZER_RUN_ENV = {
    'ASAN_OPTIONS': f'detect_leaks={_ASAN_LEAKS}:halt_on_error=1:abort_on_error=1',
    'UBSAN_OPTIONS': 'print_stacktrace=1:halt_on_error=1',
}
SOURCE_GUARD_SCRIPTS = (
    ('source-guard-c-abi', ['scripts/check_c_abi_runtime.py']),
    ('source-guard-native-source', ['scripts/test_native_source_guard.py']),
    ('source-guard-broker-hash', ['scripts/check_broker_state_hash_coverage.py']),
    ('source-guard-pending-mirror', ['scripts/gen_pending_order_mirror.py', '--check']),
    ('source-guard-native-versions', ['scripts/check_native_cpp_versions.py']),
    ('source-guard-native-c-surface', ['scripts/check_native_c_api_surface.py']),
    ('source-guard-aggregate-versions', ['scripts/check_aggregate_cpp_versions.py']),
)
NATIVE_INCLUDE_INDEPENDENCE_PROFILES = frozenset(('release', 'native', 'kernel'))
# The kernel-only archive must name no TradingView vocabulary outside ADR-0001's
# residual table (R5 gap lane P2). The archive exists in every profile and the
# CTest row test_kernel_residuals checks it there too; this stage is the
# kernel profile's own claim, run right after the build so a failing name is
# reported before the suite runs.
KERNEL_RESIDUALS_PROFILES = frozenset(('kernel',))
TWIN_PARITY_PROFILES = frozenset(('release', 'native'))
# The Pine-free hosts under examples/native/ are built, and their example_*
# ctest rows executed, in the two profiles they are written for: the default
# release build and the kernel-only one. Every example links PineForge::kernel.
EXAMPLES_PROFILES = frozenset(('release', 'kernel'))


class ConfigError(Exception):
    """CLI / profile validation; exit status 2."""


@dataclass(frozen=True)
class Profile:
    name: str
    build_type: str
    sanitizers: bool
    live_runner: bool
    tutorial: bool
    source_layer: bool
    # Minimum CTest rows the profile must run; None leaves the count ungated.
    min_tests: int | None = None


PROFILE = {
    'release': Profile('release', 'Release', False, False, True, True),
    'debug': Profile('debug', 'Debug', False, False, True, True),
    'sanitizers': Profile('sanitizers', 'Debug', True, False, True, True),
    'native': Profile('native', 'Release', False, True, False, True),
    'kernel': Profile('kernel', 'Release', False, True, False, False, KERNEL_MIN_TESTS),
}


@dataclass
class Completed:
    returncode: int
    stdout: bytes = b''
    stderr: bytes = b''


Runner = Callable[..., Completed]


@dataclass
class VerifyConfig:
    profile: Profile
    source: Path
    build_dir: Path
    jobs: int
    generator: str | None
    curl_dir: Path | None
    ccache_path: str | None
    require_websocket: bool
    runner: Runner
    stream_output: bool = True
    exclude_label: str | None = None
    # The effective CTest row floor: --min-tests, else the profile's own.
    min_tests: int | None = None


class Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise ConfigError(message)


def cmake_on(value: str | None) -> bool:
    return (value or '').strip().upper() in {'1', 'ON', 'TRUE', 'YES', 'Y'}


def expected_version(source: Path) -> str:
    return ''.join((source / 'VERSION').read_text().split())


def default_build_dir(source: Path, profile: str) -> Path:
    return source / f'build-ci-{profile}'


def call_runner(runner: Runner, argv: list[str], *, extra_env: dict[str, str] | None = None,
                timeout: int = 600, combine_stderr: bool = True,
                stream_output: bool = False) -> Completed:
    return runner(argv, extra_env=extra_env, timeout=timeout,
                  combine_stderr=combine_stderr, stream_output=stream_output)


def ctest_row_count(output: bytes) -> int | None:
    """The row count CTest prints in its closing summary line, or None."""
    match = None
    for match in CTEST_ROW_COUNT.finditer(output.decode('utf-8', 'replace')):
        pass
    return int(match.group(1)) if match else None


def ctest_supports_junit(runner: Runner) -> bool:
    try:
        result = call_runner(runner, ['ctest', '--help'], timeout=30, stream_output=False)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return '--output-junit' in (result.stdout + result.stderr).decode('utf-8', 'replace')


def source_guard_commands(source: Path) -> list[tuple[str, list[str]]]:
    python = sys.executable
    return [(name, [python, str(source / rel[0]), *rel[1:]]) for name, rel in SOURCE_GUARD_SCRIPTS]


def native_include_independence_command(cfg: VerifyConfig, prefix: Path) -> list[str]:
    argv = [sys.executable, str(cfg.source / 'scripts/check_native_include_independence.py'),
            '--build-dir', str(cfg.build_dir), '--prefix', str(prefix)]
    if not cfg.profile.source_layer:
        argv += ['--kernel-archive', str(cfg.build_dir / 'lib' / 'libpineforge_kernel.a')]
    return argv


def twin_parity_command(source: Path) -> list[str]:
    return [sys.executable, str(source / 'scripts/check_twin_parity.py')]


def kernel_residuals_command(cfg: VerifyConfig) -> list[str]:
    return [sys.executable, str(cfg.source / 'scripts/check_kernel_residuals.py'),
            '--archive', str(cfg.build_dir / 'lib' / 'libpineforge_kernel.a'),
            '--adr', str(cfg.source / 'docs/adr/0001-kernel-adapter-boundary.md')]


def cmake_cache_definitions(cfg: VerifyConfig) -> dict[str, str]:
    profile = cfg.profile
    values = {
        'CMAKE_BUILD_TYPE': profile.build_type,
        'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON',
        # Preserve the invoking virtualenv path: resolving its symlink can
        # silently select the base interpreter for CMake-registered tests.
        'Python3_EXECUTABLE': sys.executable,
        'PINEFORGE_BUILD_TESTS': 'ON',
        'PINEFORGE_BUILD_TUTORIAL': 'ON' if profile.tutorial else 'OFF',
        'PINEFORGE_BUILD_LIVE_RUNNER': 'ON' if profile.live_runner else 'OFF',
        'PINEFORGE_BUILD_SOURCE_LAYER': 'ON' if profile.source_layer else 'OFF',
        'PINEFORGE_ENABLE_SANITIZERS': 'ON' if profile.sanitizers else 'OFF',
        'PINEFORGE_BUILD_CORPUS_STRATEGIES': 'OFF',
        'PINEFORGE_BUILD_BENCH_STRATEGIES': 'OFF',
        'PINEFORGE_BUILD_SPEED_BENCH': 'OFF',
        'PINEFORGE_BUILD_EXAMPLES': 'ON' if profile.name in EXAMPLES_PROFILES else 'OFF',
        'PINEFORGE_ENABLE_COVERAGE': 'OFF',
        'PINEFORGE_STRICT_WARNINGS': 'OFF',
        'PINEFORGE_REQUIRE_ABI_RECEIPTS': 'ON',
        'PINEFORGE_VERSION_SOURCE': 'FILE',
    }
    if cfg.curl_dir is not None:
        values['CURL_DIR'] = str(cfg.curl_dir)
    if cfg.ccache_path:
        values['CMAKE_C_COMPILER_LAUNCHER'] = cfg.ccache_path
        values['CMAKE_CXX_COMPILER_LAUNCHER'] = cfg.ccache_path
    return values


def cmake_configure_argv(cfg: VerifyConfig) -> list[str]:
    argv = ['cmake', '-S', str(cfg.source), '-B', str(cfg.build_dir)]
    if cfg.generator:
        argv += ['-G', cfg.generator]
    argv += [f'-D{key}={value}' for key, value in cmake_cache_definitions(cfg).items()]
    return argv


def parse_args(argv: list[str] | None, *, source: Path = ROOT) -> argparse.Namespace:
    parser = Parser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        'profile', choices=PROFILES,
        help='release/debug keep tutorial ON and native OFF; '
             'sanitizers enable PUBLIC ASan/UBSan; native enables the live runner; '
             'kernel is native with the Pine source layer built OFF')
    parser.add_argument('--build-dir', type=Path, default=None,
                        help='default: <source>/build-ci-PROFILE')
    parser.add_argument('--jobs', type=int, default=DEFAULT_JOBS)
    parser.add_argument('--generator', default=None)
    parser.add_argument('--curl-dir', type=Path, default=None,
                        help='CURL CMake package dir (native WS-enabled curl install)')
    parser.add_argument('--ccache', action='store_true',
                        help='require installed ccache and bind CMAKE_*_COMPILER_LAUNCHER')
    parser.add_argument('--require-websocket', action='store_true',
                        help='native only: execute test_native_live_websocket and refuse skip (77)')
    parser.add_argument('--exclude-label', default=None,
                        help='exclude one CTest label from this local verification run')
    parser.add_argument('--min-tests', type=int, default=None,
                        help='fail the ctest-floor stage unless CTest ran at least N rows; '
                             f'the kernel profile defaults to {KERNEL_MIN_TESTS}, the others '
                             'to no floor')
    args = parser.parse_args(argv)
    if args.build_dir is None:
        args.build_dir = default_build_dir(source, args.profile)
    return args


def validate_config(args: argparse.Namespace, *, source: Path = ROOT,
                    which: Callable[[str], str | None] = shutil.which) -> VerifyConfig:
    if not JOBS_MIN <= args.jobs <= JOBS_MAX:
        raise ConfigError(f'--jobs must be {JOBS_MIN}..{JOBS_MAX}')
    if args.require_websocket and args.profile != 'native':
        raise ConfigError('--require-websocket is only valid with the native profile')
    if args.exclude_label is not None:
        label = args.exclude_label.strip()
        if not label or any(not (char.isalnum() or char in '_.-') for char in label):
            raise ConfigError('--exclude-label must be a simple CTest label')
        args.exclude_label = label
    if args.curl_dir is not None and not args.curl_dir.is_dir():
        raise ConfigError(f'--curl-dir is not a directory: {args.curl_dir}')
    if args.min_tests is not None and args.min_tests < 1:
        raise ConfigError('--min-tests must be at least 1')
    ccache_path = None
    if args.ccache:
        found = which('ccache')
        if not found:
            raise ConfigError('optional --ccache requires an installed ccache tool')
        ccache_path = str(Path(found).resolve())
    for tool in ('cmake', 'ctest', 'git'):
        if not which(tool):
            raise ConfigError(f'required tool not found: {tool}')
    build_dir = args.build_dir.expanduser()
    build_dir = build_dir.resolve() if build_dir.is_absolute() else (Path.cwd() / build_dir).resolve()
    source = source.resolve()
    if build_dir == source:
        raise ConfigError('--build-dir cannot be the source root')
    return VerifyConfig(
        profile=PROFILE[args.profile],
        source=source,
        build_dir=build_dir,
        jobs=args.jobs,
        generator=args.generator,
        curl_dir=args.curl_dir.resolve() if args.curl_dir is not None else None,
        ccache_path=ccache_path,
        require_websocket=bool(args.require_websocket),
        runner=default_runner,
        exclude_label=args.exclude_label,
        min_tests=args.min_tests if args.min_tests is not None else PROFILE[args.profile].min_tests,
    )


def default_runner(argv: list[str], *, extra_env: dict[str, str] | None = None,
                   timeout: int = 600, combine_stderr: bool = True,
                   stream_output: bool = True) -> Completed:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    stderr = subprocess.STDOUT if combine_stderr else subprocess.PIPE
    try:
        result = subprocess.run(
            argv, stdout=subprocess.PIPE, stderr=stderr, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        out = error.stdout or b''
        err = b'' if combine_stderr else (error.stderr or b'')
        if stream_output and out:
            sys.stdout.buffer.write(out)
            sys.stdout.buffer.flush()
        return Completed(124, out, err)
    except OSError as error:
        return Completed(127, b'', str(error).encode())
    if stream_output and result.stdout:
        sys.stdout.buffer.write(result.stdout)
        sys.stdout.buffer.flush()
    return Completed(result.returncode, result.stdout or b'',
                     b'' if combine_stderr else (result.stderr or b''))


def compile_command_text(entry: dict) -> str:
    if 'command' in entry:
        return entry['command']
    return ' '.join(entry.get('arguments') or [])


def sanitizer_flag_on_library(build_dir: Path) -> bool:
    path = build_dir / 'compile_commands.json'
    if not path.is_file():
        raise RuntimeError('compile_commands.json missing; sanitizer PUBLIC flag cannot be verified')
    entries = json.loads(path.read_text())
    library = []
    for entry in entries:
        file = (entry.get('file') or '').replace('\\', '/')
        if not file.endswith('/src/matrix.cpp') and Path(file).name != 'matrix.cpp':
            continue
        if '/src/' in file or Path(file).parent.name == 'src':
            library.append(compile_command_text(entry))
    if not library:
        raise RuntimeError('compile_commands.json has no src/matrix.cpp entry')
    return all(SANITIZER_FLAG in command for command in library)


def stale_archive_sources(source: Path, archive: Path) -> list[str]:
    if not archive.is_file():
        raise RuntimeError(f'full build did not produce {archive}')
    newest = archive.stat().st_mtime
    stale = []
    for root in (source / 'CMakeLists.txt', source / 'src', source / 'include'):
        paths = [root] if root.is_file() else [path for path in root.rglob('*') if path.is_file()]
        for path in paths:
            if path.stat().st_mtime > newest:
                stale.append(str(path.relative_to(source)))
    return stale


def smoke_prefix(cache: dict[str, str], install_prefix: Path) -> str:
    parts = [str(install_prefix)]
    extra = cache.get('CMAKE_PREFIX_PATH')
    if extra:
        for item in extra.split(';'):
            if item and item not in parts:
                parts.append(item)
    eigen = cache.get('Eigen3_DIR')
    if eigen and eigen not in parts:
        parts.append(eigen)
    return ';'.join(parts)


def pinned_object_present(source: Path, runner: Runner, commit: str = BASE_COMMIT) -> bool:
    result = call_runner(
        runner, ['git', '-C', str(source), 'cat-file', '-e', commit + '^{commit}'],
        timeout=30, stream_output=False)
    return result.returncode == 0


def tool_version_commands(cfg: VerifyConfig) -> list[list[str]]:
    commands = [
        ['cmake', '--version'],
        ['ctest', '--version'],
        ['git', '--version'],
        [sys.executable, '--version'],
    ]
    compiler = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
    if compiler:
        commands.append([compiler, '--version'])
    if cfg.ccache_path:
        commands.append([cfg.ccache_path, '--version'])
    return commands


class Driver:
    def __init__(self, cfg: VerifyConfig):
        self.cfg = cfg
        self.logs = cfg.build_dir / 'ci-logs'
        self.install_prefix = cfg.build_dir / 'ci-install'
        self.smoke_dir = cfg.build_dir / 'ci-smoke'
        self.summary_path = cfg.build_dir / 'ci-summary.json'
        self.failures: list[dict] = []
        self.stages: list[dict] = []
        self.expected_version = expected_version(cfg.source)
        self.actual_version: str | None = None
        self.abi_action = 'not-started'
        self.abi_prior_action = 'not-started'
        self.abi_v13_action = 'not-started'
        self.abi_v14_action = 'not-started'
        self.abi_v15_frozen_action = 'not-started'
        self.abi_v16_frozen_action = 'not-started'
        self.summary: dict = {
            'schemaVersion': SCHEMA,
            'status': 'incomplete',
            'profile': cfg.profile.name,
            'source': str(cfg.source),
            'buildDir': str(cfg.build_dir),
            'installPrefix': str(self.install_prefix),
            'smokeDir': str(self.smoke_dir),
            'jobs': cfg.jobs,
            'generator': cfg.generator,
            'ccache': cfg.ccache_path,
            'requireWebsocket': cfg.require_websocket,
            'curlDir': str(cfg.curl_dir) if cfg.curl_dir else None,
            'minTests': cfg.min_tests,
            'ctestRows': None,
            'versionSource': 'FILE',
            'cmakeDefinitions': cmake_cache_definitions(cfg),
            'expectedVersion': self.expected_version,
            'actualVersion': None,
            'exitCode': None,
            'abi': {'action': self.abi_action},
            'abiPrior': {'action': self.abi_prior_action},
            'abiV13': {'action': self.abi_v13_action},
            'abiV14': {'action': self.abi_v14_action},
            'abiV15Frozen': {'action': self.abi_v15_frozen_action},
            'abiV16Frozen': {'action': self.abi_v16_frozen_action},
            'stages': self.stages,
            'failures': self.failures,
        }

    def write_summary(self) -> None:
        self.summary['abi'] = {'action': self.abi_action}
        self.summary['abiPrior'] = {'action': self.abi_prior_action}
        self.summary['abiV13'] = {'action': self.abi_v13_action}
        self.summary['abiV14'] = {'action': self.abi_v14_action}
        self.summary['abiV15Frozen'] = {'action': self.abi_v15_frozen_action}
        self.summary['abiV16Frozen'] = {'action': self.abi_v16_frozen_action}
        self.summary['actualVersion'] = self.actual_version
        self.summary['stages'] = self.stages
        self.summary['failures'] = self.failures
        self.summary_path.parent.mkdir(parents=True, exist_ok=True)
        self.summary_path.write_text(json.dumps(self.summary, indent=2, sort_keys=True) + '\n')

    def invoke(self, name: str, argv: list[str], *, extra_env: dict[str, str] | None = None,
               timeout: int = 600, combine_stderr: bool = True) -> Completed:
        if self.cfg.ccache_path:
            extra_env = {**(extra_env or {}), 'CCACHE_COMPILERCHECK': 'content'}
        log = self.logs / f'{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        started = time.time()
        header = '+ ' + shlex.join(map(str, argv)) + '\n'
        stage = {
            'name': name, 'argv': list(map(str, argv)), 'exitCode': None,
            'status': 'running', 'log': str(log.relative_to(self.cfg.build_dir)),
            'extraEnvKeys': sorted(extra_env) if extra_env else [],
        }
        self.stages.append(stage)
        log.write_text(header)
        self.write_summary()
        if self.cfg.stream_output:
            print(f'ci_verify: {name}: {shlex.join(map(str, argv))}', flush=True)
        try:
            result = call_runner(
                self.cfg.runner, list(map(str, argv)), extra_env=extra_env, timeout=timeout,
                combine_stderr=combine_stderr, stream_output=self.cfg.stream_output)
        except Exception as error:
            result = Completed(1, b'', str(error).encode())
        body = result.stdout + (b'' if not result.stderr else b'\n' + result.stderr)
        log.write_bytes(header.encode() + body)
        stage.update({
            'exitCode': result.returncode,
            'status': 'passed' if result.returncode == 0 else 'failed',
            'durationSeconds': round(time.time() - started, 3),
        })
        if result.returncode != 0:
            self.failures.append({
                'stage': name,
                'exitCode': result.returncode,
                'log': stage['log'],
            })
        self.write_summary()
        return result

    def fail_stage(self, name: str, message: str, *, argv: list[str] | None = None) -> None:
        log = self.logs / f'{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(message + '\n')
        self.stages.append({
            'name': name,
            'argv': argv or [],
            'exitCode': 1,
            'status': 'failed',
            'log': str(log.relative_to(self.cfg.build_dir)),
            'error': message,
            'extraEnvKeys': [],
        })
        self.failures.append({
            'stage': name, 'exitCode': 1, 'error': message,
            'log': str(log.relative_to(self.cfg.build_dir)),
        })
        self.write_summary()

    def pass_stage(self, name: str, message: str, *, argv: list[str] | None = None) -> None:
        log = self.logs / f'{name}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(message + '\n')
        self.stages.append({
            'name': name,
            'argv': argv or [],
            'exitCode': 0,
            'status': 'passed',
            'log': str(log.relative_to(self.cfg.build_dir)),
            'extraEnvKeys': [],
        })
        self.write_summary()

    def sanitizer_env(self) -> dict[str, str] | None:
        return dict(SANITIZER_RUN_ENV) if self.cfg.profile.sanitizers else None

    def collect_tool_versions(self) -> bool:
        chunks = []
        failed = False
        for argv in tool_version_commands(self.cfg):
            result = call_runner(
                self.cfg.runner, argv, timeout=30, stream_output=False)
            chunks.append('+ ' + shlex.join(argv))
            chunks.append((result.stdout + result.stderr).decode('utf-8', 'replace'))
            if result.returncode != 0 and Path(argv[0]).name in {'cmake', 'ctest', 'git'}:
                failed = True
        self.logs.mkdir(parents=True, exist_ok=True)
        (self.logs / 'tool-versions.log').write_text('\n'.join(chunks) + '\n')
        self.stages.append({
            'name': 'tool-versions',
            'argv': ['tool-versions'],
            'exitCode': 1 if failed else 0,
            'status': 'failed' if failed else 'passed',
            'log': 'ci-logs/tool-versions.log',
            'extraEnvKeys': [],
        })
        if failed:
            self.failures.append({'stage': 'tool-versions', 'exitCode': 1})
        self.write_summary()
        return not failed

    def ensure_corpus_submodule(self) -> bool:
        """Materialize the exact public corpus gitlink before sweep-adjacent CI."""
        update = [
            'git', '-C', str(self.cfg.source), 'submodule', 'update', '--init',
            '--depth', '1', '--', 'corpus',
        ]
        if self.invoke('corpus-submodule-init', update, timeout=600).returncode != 0:
            return False
        status = self.invoke(
            'corpus-submodule-status',
            ['git', '-C', str(self.cfg.source), 'submodule', 'status', '--', 'corpus'],
            timeout=60)
        if status.returncode != 0:
            return False
        value = status.stdout.decode('utf-8', 'replace').strip()
        if not value or value[0] in '-+':
            self.fail_stage(
                'corpus-submodule-pin',
                'corpus submodule is absent or not at the recorded gitlink: ' + repr(value),
                argv=['git', 'submodule', 'status', '--', 'corpus'])
            return False
        self.pass_stage('corpus-submodule-pin', value,
                        argv=['git', 'submodule', 'status', '--', 'corpus'])
        return True

    def verify_configured_profile(self, cache: dict[str, str]) -> str | None:
        profile = self.cfg.profile
        if cache.get('CMAKE_BUILD_TYPE') != profile.build_type:
            return (f'CMAKE_BUILD_TYPE expected {profile.build_type!r} '
                    f'got {cache.get("CMAKE_BUILD_TYPE")!r}')
        if cache.get('PINEFORGE_VERSION_SOURCE') != 'FILE':
            return (f'PINEFORGE_VERSION_SOURCE expected FILE '
                    f'got {cache.get("PINEFORGE_VERSION_SOURCE")!r}')
        if cache.get('Python3_EXECUTABLE') != sys.executable:
            return (f'Python3_EXECUTABLE expected {sys.executable!r} '
                    f'got {cache.get("Python3_EXECUTABLE")!r}')
        for key, wanted in (
            ('PINEFORGE_BUILD_TESTS', True),
            ('PINEFORGE_BUILD_TUTORIAL', profile.tutorial),
            ('PINEFORGE_BUILD_LIVE_RUNNER', profile.live_runner),
            ('PINEFORGE_BUILD_SOURCE_LAYER', profile.source_layer),
            ('PINEFORGE_ENABLE_SANITIZERS', profile.sanitizers),
            ('PINEFORGE_BUILD_EXAMPLES', profile.name in EXAMPLES_PROFILES),
            ('PINEFORGE_REQUIRE_ABI_RECEIPTS', True),
        ):
            if cmake_on(cache.get(key)) != wanted:
                return f'{key} expected {"ON" if wanted else "OFF"} got {cache.get(key)!r}'
        if self.cfg.ccache_path:
            for launcher in ('CMAKE_C_COMPILER_LAUNCHER', 'CMAKE_CXX_COMPILER_LAUNCHER'):
                if cache.get(launcher) != self.cfg.ccache_path:
                    return f'{launcher} expected {self.cfg.ccache_path!r} got {cache.get(launcher)!r}'
        return None

    def ensure_abi_base(self) -> None:
        self.abi_action = self.ensure_prepared_provider(
            self.cfg.build_dir / 'settlement-abi-base', BASE_COMMIT, BASE_TREE,
            extra_argv=[], stage='abi-base', fetch_stage='abi-fetch')

    def ensure_abi_prior(self) -> None:
        provider = PROVIDERS['0e']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_prior_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-prior', fetch_stage='abi-prior-fetch')

    def ensure_abi_v13(self) -> None:
        provider = PROVIDERS['v13']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v13_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v13', fetch_stage='abi-v13-fetch')

    def ensure_abi_v14(self) -> None:
        provider = PROVIDERS['v14']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v14_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v14', fetch_stage='abi-v14-fetch')

    def ensure_abi_v15_frozen(self) -> None:
        provider = PROVIDERS['v15-frozen']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v15_frozen_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v15-frozen', fetch_stage='abi-v15-frozen-fetch')

    def ensure_abi_v16_frozen(self) -> None:
        provider = PROVIDERS['v16-frozen']
        manifest = self.cfg.source / provider['manifest'].relative_to(ROOT)
        self.abi_v16_frozen_action = self.ensure_prepared_provider(
            self.cfg.build_dir / provider['default_output'], provider['commit'], provider['tree'],
            extra_argv=['--commit', provider['commit'], '--tree', provider['tree'],
                        '--header-manifest', str(manifest)],
            stage='abi-v16-frozen', fetch_stage='abi-v16-frozen-fetch')

    def ensure_prepared_provider(self, output: Path, commit: str, tree: str, *,
                                 extra_argv: list[str], stage: str, fetch_stage: str) -> str:
        prepare = [
            sys.executable, str(self.cfg.source / 'scripts/prepare_settlement_cpp_abi_base.py'),
            '--source-repo', str(self.cfg.source),
            '--current-build', str(self.cfg.build_dir),
            '--output', str(output),
            '--jobs', str(self.cfg.jobs),
            *extra_argv,
        ]
        if output.exists():
            try:
                receipt = reusable_prepared_base(output, self.cfg.build_dir, commit=commit, tree=tree)
            except Exception as error:
                self.fail_stage(stage, str(error), argv=['reuse-matching-receipt', str(output)])
                return 'refused'
            self.pass_stage(
                stage,
                json.dumps({'action': 'reused', 'receipt': str(output / 'receipt.json'),
                            'archiveSha256': receipt.get('archiveSha256')}, indent=2, sort_keys=True),
                argv=['reuse-matching-receipt', str(output / 'receipt.json')])
            return 'reused'
        if not pinned_object_present(self.cfg.source, self.cfg.runner, commit):
            fetch = ['git', '-C', str(self.cfg.source), 'fetch', '--no-tags', '--depth=1',
                     'origin', commit]
            fetched = self.invoke(fetch_stage, fetch, timeout=120)
            if fetched.returncode != 0:
                return 'failed'
        prepared = self.invoke(stage, prepare, timeout=1800)
        return 'prepared' if prepared.returncode == 0 else 'failed'

    def run_smoke(self, cache: dict[str, str]) -> None:
        prefix = smoke_prefix(cache, self.install_prefix)
        configure = [
            'cmake', '-S', str(self.cfg.source / 'cmake/smoke_consumer'),
            '-B', str(self.smoke_dir),
            f'-DCMAKE_BUILD_TYPE={self.cfg.profile.build_type}',
            f'-DCMAKE_PREFIX_PATH={prefix}',
        ]
        if cache.get('Eigen3_DIR'):
            configure.append(f'-DEigen3_DIR={cache["Eigen3_DIR"]}')
        if self.invoke('smoke-configure', configure, timeout=120).returncode != 0:
            return
        if self.invoke(
                'smoke-build',
                ['cmake', '--build', str(self.smoke_dir), '--parallel', str(self.cfg.jobs)],
                timeout=300).returncode != 0:
            return
        binary = self.smoke_dir / 'smoke_version'
        ran = self.invoke(
            'smoke-version', [str(binary)], extra_env=self.sanitizer_env(),
            timeout=60, combine_stderr=False)
        actual = ran.stdout.decode('utf-8', 'replace').strip()
        self.actual_version = actual
        self.summary['actualVersion'] = actual
        if ran.returncode != 0:
            self.failures[-1]['error'] = (
                f'version smoke exited {ran.returncode}; expected {self.expected_version!r} '
                f'got {actual!r}')
            self.write_summary()
            return
        if actual != self.expected_version:
            self.fail_stage(
                'smoke-version-check',
                f'version smoke expected {self.expected_version!r} got {actual!r}',
                argv=[str(binary)])
            return
        self.pass_stage(
            'smoke-version-check',
            f'printed VERSION {actual} matches {self.cfg.source / "VERSION"}',
            argv=[str(binary)])

    def enforce_native_include_independence(self) -> bool:
        if self.cfg.profile.name not in NATIVE_INCLUDE_INDEPENDENCE_PROFILES:
            return True
        with tempfile.TemporaryDirectory(prefix='pineforge-native-include-') as temporary:
            result = self.invoke(
                'native-include-independence',
                native_include_independence_command(self.cfg, Path(temporary)),
                timeout=300)
        return result.returncode == 0

    def enforce_kernel_residuals(self) -> bool:
        if self.cfg.profile.name not in KERNEL_RESIDUALS_PROFILES:
            return True
        result = self.invoke('kernel-residuals', kernel_residuals_command(self.cfg), timeout=300)
        return result.returncode == 0

    def run(self) -> int:
        self.logs.mkdir(parents=True, exist_ok=True)
        self.write_summary()
        if not self.collect_tool_versions():
            return self.finish('failed', 1)
        if not self.ensure_corpus_submodule():
            return self.finish('failed', 1)
        guard_failed = False
        for name, argv in source_guard_commands(self.cfg.source):
            if self.invoke(name, argv, timeout=120).returncode != 0:
                guard_failed = True
        if self.cfg.profile.name in TWIN_PARITY_PROFILES:
            if self.invoke('source-guard-twin-parity',
                           twin_parity_command(self.cfg.source), timeout=120).returncode != 0:
                guard_failed = True
        if guard_failed:
            return self.finish('failed', 1)
        if self.invoke('configure', cmake_configure_argv(self.cfg), timeout=180).returncode != 0:
            return self.finish('failed', 1)
        cache_path = self.cfg.build_dir / 'CMakeCache.txt'
        if not cache_path.is_file():
            self.fail_stage('configure-cache', f'missing {cache_path}')
            return self.finish('failed', 1)
        cache = read_cache(cache_path)
        profile_error = self.verify_configured_profile(cache)
        if profile_error:
            self.fail_stage('profile-options', profile_error)
            return self.finish('failed', 1)
        self.pass_stage('profile-options', json.dumps({
            'tutorial': cmake_on(cache.get('PINEFORGE_BUILD_TUTORIAL')),
            'liveRunner': cmake_on(cache.get('PINEFORGE_BUILD_LIVE_RUNNER')),
            'sourceLayer': cmake_on(cache.get('PINEFORGE_BUILD_SOURCE_LAYER')),
            'sanitizers': cmake_on(cache.get('PINEFORGE_ENABLE_SANITIZERS')),
            'examples': cmake_on(cache.get('PINEFORGE_BUILD_EXAMPLES')),
            'versionSource': cache.get('PINEFORGE_VERSION_SOURCE'),
            'python': cache.get('Python3_EXECUTABLE'),
            'buildType': cache.get('CMAKE_BUILD_TYPE'),
        }, indent=2, sort_keys=True))
        if self.cfg.profile.sanitizers:
            try:
                if not sanitizer_flag_on_library(self.cfg.build_dir):
                    self.fail_stage(
                        'sanitizer-public-flag',
                        f'src/matrix.cpp compile command missing PUBLIC {SANITIZER_FLAG}')
                    return self.finish('failed', 1)
            except Exception as error:
                self.fail_stage('sanitizer-public-flag', str(error))
                return self.finish('failed', 1)
            self.pass_stage('sanitizer-public-flag', f'library compile uses {SANITIZER_FLAG}')
        if self.invoke(
                'build',
                ['cmake', '--build', str(self.cfg.build_dir), '--parallel', str(self.cfg.jobs)],
                timeout=1800).returncode != 0:
            return self.finish('failed', 1)
        archive = self.cfg.build_dir / 'lib' / 'libpineforge.a'
        try:
            stale = stale_archive_sources(self.cfg.source, archive)
        except Exception as error:
            self.fail_stage('stale-binaries', str(error))
            return self.finish('failed', 1)
        if stale:
            self.fail_stage(
                'stale-binaries',
                'libpineforge.a predates source; full rebuild required: ' + ', '.join(stale[:40]))
            return self.finish('failed', 1)
        self.pass_stage('stale-binaries', f'{archive} is newer than src/, include/, CMakeLists.txt')
        if not self.enforce_native_include_independence():
            return self.finish('failed', 1)
        if not self.enforce_kernel_residuals():
            return self.finish('failed', 1)
        live = self.cfg.build_dir / 'bin' / 'pineforge-live'
        if self.cfg.profile.live_runner:
            if not live.is_file():
                self.fail_stage('native-binary', f'native profile missing {live}')
                return self.finish('failed', 1)
            self.pass_stage('native-binary', str(live))
        elif live.is_file():
            self.fail_stage(
                'native-binary',
                'non-native profile produced pineforge-live; live runner must stay OFF')
            return self.finish('failed', 1)
        else:
            self.pass_stage('native-binary', 'live runner absent as required for this profile')

        if self.cfg.profile.source_layer:
            self.ensure_abi_base()
            self.ensure_abi_prior()
            self.ensure_abi_v13()
            self.ensure_abi_v14()
            self.ensure_abi_v15_frozen()
            self.ensure_abi_v16_frozen()
        else:
            # Every receipt-backed row pairs through a source::PineStrategyHost
            # TU, so the kernel-only build registers none of them.
            self.pass_stage('abi-providers-skipped',
                            'kernel-only build registers no receipt-backed ABI row')

        # AppleClang's ASan runtime serializes shadow-memory initialization
        # behind a process-global spin lock. Starting several instrumented
        # binaries at once can wedge them before main(). Keep an AppleClang
        # Darwin sanitizer lane serial; a caller that explicitly selects a
        # GNU g++ runtime can retain normal parallelism, as can Linux CI.
        cxx_name = Path(os.environ.get('CXX', '')).name
        apple_asan = (self.cfg.profile.sanitizers and sys.platform == 'darwin'
                      and not cxx_name.startswith('g++'))
        ctest_jobs = 1 if apple_asan else self.cfg.jobs
        ctest = ['ctest', '--test-dir', str(self.cfg.build_dir),
                 '--output-on-failure', '--no-tests=error', '--parallel', str(ctest_jobs)]
        if self.cfg.exclude_label:
            ctest += ['-LE', self.cfg.exclude_label]
        if ctest_supports_junit(self.cfg.runner):
            ctest += ['--output-junit', str(self.cfg.build_dir / 'ctest-junit.xml')]
        ran = self.invoke('ctest', ctest, extra_env=self.sanitizer_env(), timeout=1800)
        self.enforce_test_floor(ran)

        installed = self.invoke(
            'install',
            ['cmake', '--install', str(self.cfg.build_dir), '--prefix', str(self.install_prefix)],
            timeout=180)
        if installed.returncode == 0:
            self.run_smoke(cache)
            if self.cfg.profile.live_runner:
                help_bin = self.install_prefix / 'bin' / 'pineforge-live'
                if not help_bin.is_file():
                    self.fail_stage('native-help', f'installed native executable missing: {help_bin}')
                else:
                    self.invoke('native-help', [str(help_bin), '--help'], timeout=30)
        if self.cfg.require_websocket:
            ws = self.cfg.build_dir / 'bin' / 'test_native_live_websocket'
            if not ws.is_file():
                self.fail_stage(
                    'require-websocket',
                    f'--require-websocket cannot accept skip: missing {ws}')
            else:
                ran = self.invoke(
                    'require-websocket', [str(ws)], extra_env=self.sanitizer_env(), timeout=120)
                if ran.returncode == 77:
                    self.fail_stage(
                        'require-websocket-skip',
                        '--require-websocket cannot accept skip (exit 77)',
                        argv=[str(ws)])
        status = 'passed' if not self.failures else 'failed'
        return self.finish(status, 0 if status == 'passed' else 1)

    def enforce_test_floor(self, ran: Completed) -> None:
        """Refuse a CTest run that shrank below the profile's row floor.

        The count is CTest's own closing summary; a run that prints none
        (no tests found, a crash before the summary) fails closed rather
        than passing an empty or truncated suite through the floor.
        """
        count = ctest_row_count(ran.stdout + ran.stderr)
        self.summary['ctestRows'] = count
        floor = self.cfg.min_tests
        if floor is None:
            self.write_summary()
            return
        if count is None:
            self.fail_stage(
                'ctest-floor',
                f'ctest printed no row count; the floor of {floor} rows cannot be verified')
        elif count < floor:
            self.fail_stage(
                'ctest-floor',
                f'ctest ran {count} rows, below the floor of {floor} for the '
                f'{self.cfg.profile.name} profile; a source-free test left the suite, '
                'or lower the floor with --min-tests on purpose')
        else:
            self.pass_stage('ctest-floor', f'ctest ran {count} rows (floor {floor})')

    def finish(self, status: str, code: int) -> int:
        self.summary['status'] = status
        self.summary['exitCode'] = code
        self.write_summary()
        return code


def build_config(argv: list[str] | None = None, *, source: Path = ROOT,
                 runner: Runner | None = None, stream_output: bool | None = None,
                 which: Callable[[str], str | None] = shutil.which) -> VerifyConfig:
    args = parse_args(argv, source=source)
    cfg = validate_config(args, source=source, which=which)
    if runner is not None:
        cfg.runner = runner
    if stream_output is not None:
        cfg.stream_output = stream_output
    return cfg


def main(argv: list[str] | None = None, *, runner: Runner | None = None,
         stream_output: bool | None = None,
         which: Callable[[str], str | None] = shutil.which) -> int:
    try:
        cfg = build_config(argv, runner=runner, stream_output=stream_output, which=which)
    except ConfigError as error:
        print(f'ci_verify: {error}', file=sys.stderr)
        return 2
    driver = Driver(cfg)
    try:
        return driver.run()
    except ConfigError as error:
        print(f'ci_verify: {error}', file=sys.stderr)
        driver.fail_stage('config', str(error))
        return driver.finish('failed', 2)
    except Exception as error:
        driver.fail_stage('driver', str(error))
        return driver.finish('failed', 1)


if __name__ == '__main__':
    raise SystemExit(main())
