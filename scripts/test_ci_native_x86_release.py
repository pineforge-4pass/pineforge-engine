#!/usr/bin/env python3
"""Unit tests for scripts/ci_native_x86_release.py, the helper behind the
required native x86-64 GNU 13.3.0 Release build job of gke-pr.yml and
ci-post.yml. A ci_preflight stage runs them.

The helper's process and PATH transport is replaced by FakeHost, which answers
only the argv the helper is specified to issue and fails on anything else. Its
cmake writes the CMake cache, the generated compiler descriptions and
compile_commands.json from the -D arguments the helper really passed, so a
dropped, renamed or altered flag changes the verdict. These tests prove the
helper's toolchain selection, argv, ordering, exit-code and evidence handling.
They are not an engine build: the compile itself runs only in the workflow job
on its GKE runner."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ci_native_x86_release as helper  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
GOOD_BANNER = 'gcc (Ubuntu 13.3.0-1ubuntu1) 13.3.0'
HEAD = '0123456789abcdef0123456789abcdef01234567'


def expected_definitions(cc: str, cxx: str) -> list[str]:
    """The configure flags the required job is specified to pass, spelled out
    independently of the helper's own list."""
    return [
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_CXX_STANDARD=17',
        '-DCMAKE_CXX_STANDARD_REQUIRED=ON',
        '-DCMAKE_CXX_EXTENSIONS=OFF',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        f'-DCMAKE_C_COMPILER={cc}',
        f'-DCMAKE_CXX_COMPILER={cxx}',
        '-DPINEFORGE_VERSION_SOURCE=FILE',
        '-DPINEFORGE_BUILD_TESTS=OFF',
        '-DPINEFORGE_BUILD_SOURCE_LAYER=ON',
        '-DPINEFORGE_BUILD_EXAMPLES=OFF',
        '-DPINEFORGE_BUILD_CORPUS_STRATEGIES=OFF',
        '-DPINEFORGE_BUILD_TUTORIAL=OFF',
        '-DPINEFORGE_BUILD_LIVE_RUNNER=OFF',
        '-DPINEFORGE_BUILD_BENCH_STRATEGIES=OFF',
        '-DPINEFORGE_BUILD_SPEED_BENCH=OFF',
        '-DPINEFORGE_ENABLE_COVERAGE=OFF',
        '-DPINEFORGE_ENABLE_SANITIZERS=OFF',
        '-DPINEFORGE_STRICT_WARNINGS=OFF',
        '-DPINEFORGE_REQUIRE_ABI_RECEIPTS=OFF',
    ]


def write_description(build: Path, language: str, settings: dict[str, str],
                      version_dir: str = '3.28.3') -> None:
    """Write CMake<language>Compiler.cmake where CMake keeps it."""
    folder = build / 'CMakeFiles' / version_dir
    folder.mkdir(parents=True, exist_ok=True)
    lines = [f'set({name} "{value}")' for name, value in settings.items()]
    # Neighbouring settings CMake writes that a reader must not mistake for
    # the compiler path, identity or version.
    lines += [f'set(CMAKE_{language}_COMPILER_ARG1 "")',
              f'set(CMAKE_{language}_COMPILER_VERSION_INTERNAL "0.0.0")',
              f'set(CMAKE_{language}_COMPILER_ID_RUN 1)',
              f'set(CMAKE_{language}_COMPILER_LOADED 1)']
    (folder / f'CMake{language}Compiler.cmake').write_text(
        '\n'.join(lines) + '\n', encoding='utf-8')


def write_cmake_tree(build: Path, cache: dict[str, str],
                     descriptions: dict[str, dict[str, str]],
                     version_dir: str = '3.28.3') -> None:
    """Write the cache and the compiler descriptions of a configured tree."""
    build.mkdir(parents=True, exist_ok=True)
    lines = ['# This is the CMakeCache file.', '# KEY:TYPE=VALUE', '']
    for name, value in cache.items():
        kind = 'INTERNAL' if name == 'CMAKE_GENERATOR' else 'UNINITIALIZED'
        lines += [f'//Value of {name}', f'{name}:{kind}={value}']
    (build / 'CMakeCache.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    for language, settings in descriptions.items():
        write_description(build, language, settings, version_dir)


def good_tree(cc: str, cxx: str) -> tuple[dict[str, str], dict[str, dict[str, str]]]:
    """The cache and descriptions of a configure that is exactly as required."""
    cache = {'CMAKE_C_COMPILER': cc, 'CMAKE_CXX_COMPILER': cxx,
             'CMAKE_GENERATOR': 'Unix Makefiles', 'CMAKE_BUILD_TYPE': 'Release',
             'CMAKE_CXX_STANDARD': '17'}
    descriptions = {
        'C': {'CMAKE_C_COMPILER': cc, 'CMAKE_C_COMPILER_ID': 'GNU',
              'CMAKE_C_COMPILER_VERSION': '13.3.0'},
        'CXX': {'CMAKE_CXX_COMPILER': cxx, 'CMAKE_CXX_COMPILER_ID': 'GNU',
                'CMAKE_CXX_COMPILER_VERSION': '13.3.0'},
    }
    return cache, descriptions


def toolchain_at(cc: str, cxx: str, target: str = 'x86_64-linux-gnu') -> helper.Toolchain:
    return helper.Toolchain(
        helper.Compiler('C', Path(cc), target, '13.3.0', 'gcc banner'),
        helper.Compiler('CXX', Path(cxx), target, '13.3.0', 'g++ banner'))


class FakeHost:
    """The machine the helper believes it runs on.

    Only the argv the helper is specified to issue are answered; anything else
    raises AssertionError. cmake configure derives the tree it writes from the
    -D arguments it was given, never from constants.
    """

    def __init__(self, tmp: Path, *, machine: str = 'x86_64') -> None:
        self.tmp = tmp
        self.uname = (0, machine + '\n', '')
        self.git = {'rev-parse': HEAD + '\n', 'status': ' M scripts/example.py\n'}
        self.paths: dict[str, str] = {}
        self.tools: dict[tuple[str, str], object] = {}
        self.reported: dict[str, str] = {}
        self.detected: dict[str, tuple[str, str]] = {}
        self.cache_overrides: dict[str, str | None] = {}
        self.configure_exit = 0
        self.build_exit = 0
        self.launch_error: dict[str, BaseException] = {}
        self.write_compile_commands = True
        self.calls: list[tuple[list[str], dict]] = []

    def compiler(self, name: str, *, target: str = 'x86_64-linux-gnu',
                 version: str = '13.3.0', banner: str = GOOD_BANNER) -> str:
        path = str(self.tmp / 'toolchain' / name)
        self.paths[name] = path
        self.reported[path] = version
        self.tools[(path, '-dumpmachine')] = (0, target + '\n', '')
        self.tools[(path, '-dumpfullversion')] = (0, version + '\n', '')
        self.tools[(path, '--version')] = (0, banner + '\n', '')
        return path

    def find(self, name, *args, **kwargs):
        return self.paths.get(name)

    @contextlib.contextmanager
    def installed(self):
        with mock.patch.object(helper.subprocess, 'run', self.run), \
                mock.patch.object(helper.shutil, 'which', self.find):
            yield self

    @staticmethod
    def done(argv, code, out=None, err=None):
        return subprocess.CompletedProcess(argv, code, out, err)

    def run(self, argv, **kwargs):
        argv = [str(part) for part in argv]
        self.calls.append((argv, kwargs))
        if argv == ['uname', '-m']:
            return self.done(argv, *self.uname)
        if argv[0] == 'git':
            return self.done(argv, 0, self.git[argv[1]], '')
        if argv[0] == 'cmake':
            return self.cmake(argv, kwargs)
        answer = self.tools.get((argv[0], argv[1])) if len(argv) == 2 else None
        if answer is None:
            raise AssertionError(f'unmodelled process: {argv}')
        if isinstance(answer, BaseException):
            raise answer
        return self.done(argv, *answer)

    def cmake(self, argv, kwargs):
        phase = 'build' if argv[1] == '--build' else 'configure'
        if phase in self.launch_error:
            raise self.launch_error[phase]
        kwargs['stdout'].write(f'{phase} stdout line\n')
        kwargs['stderr'].write(f'{phase} stderr line\n')
        code = self.build_exit if phase == 'build' else self.configure_exit
        if phase == 'configure' and code == 0:
            self.configured(argv)
        return self.done(argv, code)

    def configured(self, argv):
        build = Path(argv[argv.index('-B') + 1])
        defs = dict(part[2:].split('=', 1) for part in argv if part.startswith('-D'))
        cache = {**defs, 'CMAKE_GENERATOR': argv[argv.index('-G') + 1]}
        for name, value in self.cache_overrides.items():
            if value is None:
                cache.pop(name, None)
            else:
                cache[name] = value
        descriptions = {}
        for language in ('C', 'CXX'):
            path = defs.get(f'CMAKE_{language}_COMPILER', '')
            compiler_id, version = self.detected.get(
                path, ('GNU', self.reported.get(path, '')))
            descriptions[language] = {
                f'CMAKE_{language}_COMPILER': path,
                f'CMAKE_{language}_COMPILER_ID': compiler_id,
                f'CMAKE_{language}_COMPILER_VERSION': version,
            }
        write_cmake_tree(build, cache, descriptions)
        if defs.get('CMAKE_EXPORT_COMPILE_COMMANDS') == 'ON' and self.write_compile_commands:
            entries = [{'directory': str(build), 'file': 'main.cpp',
                        'command': f"{defs.get('CMAKE_CXX_COMPILER', '')} -c main.cpp"}]
            (build / 'compile_commands.json').write_text(
                json.dumps(entries) + '\n', encoding='utf-8')


class Outcome:
    """One run of helper.main and the artifact directory it left."""

    def __init__(self, code, out, err, source, build, artifacts) -> None:
        self.code, self.out, self.err = code, out, err
        self.source, self.build, self.artifacts = source, build, artifacts

    def read(self, name: str) -> str:
        return (self.artifacts / name).read_text(encoding='utf-8')

    def has(self, name: str) -> bool:
        return (self.artifacts / name).exists()


class TempTestCase(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.tmp = Path(directory.name).resolve()
        self.counter = 0

    def fresh(self) -> Path:
        self.counter += 1
        path = self.tmp / f'case-{self.counter}'
        path.mkdir()
        return path


class ToolchainSelectionTests(TempTestCase):
    def install_pairs(self, host: FakeHost, versions: list[str]) -> None:
        for (c_name, cxx_name), version in zip(helper.COMPILER_PAIRS, versions):
            host.compiler(c_name, version=version)
            host.compiler(cxx_name, version=version)

    def test_the_first_pair_that_passes_every_check_is_the_one_selected(self):
        count = len(helper.COMPILER_PAIRS)
        for good in range(count):
            with self.subTest(good_pair=helper.COMPILER_PAIRS[good]):
                host = FakeHost(self.fresh())
                self.install_pairs(
                    host, ['13.3.0' if index == good else '12.4.0' for index in range(count)])
                with host.installed():
                    toolchain = helper.select_toolchain()
                c_name, cxx_name = helper.COMPILER_PAIRS[good]
                self.assertEqual(toolchain.c.path, Path(host.paths[c_name]))
                self.assertEqual(toolchain.cxx.path, Path(host.paths[cxx_name]))
                self.assertEqual((toolchain.c.language, toolchain.cxx.language), ('C', 'CXX'))
                self.assertEqual((toolchain.c.version, toolchain.cxx.version),
                                 ('13.3.0', '13.3.0'))
                self.assertEqual(toolchain.c.target, 'x86_64-linux-gnu')
                self.assertEqual(toolchain.cxx.banner, GOOD_BANNER)

    def test_a_pair_missing_one_driver_is_skipped_rather_than_half_used(self):
        host = FakeHost(self.fresh())
        first_c, _first_cxx = helper.COMPILER_PAIRS[0]
        second_c, second_cxx = helper.COMPILER_PAIRS[1]
        host.compiler(first_c)
        host.compiler(second_c)
        host.compiler(second_cxx)
        with host.installed():
            toolchain = helper.select_toolchain()
        self.assertEqual(toolchain.c.path, Path(host.paths[second_c]))
        self.assertEqual(toolchain.cxx.path, Path(host.paths[second_cxx]))

    REFUSALS = (
        ('older version', {'version': '12.4.0'}, "'12.4.0'"),
        ('newer patch', {'version': '13.3.1'}, "'13.3.1'"),
        ('truncated version', {'version': '13.3'}, "'13.3'"),
        ('suffixed version', {'version': '13.3.0-pre'}, "'13.3.0-pre'"),
        ('empty version', {'version': ''}, "reports GCC ''"),
        ('arm target', {'target': 'aarch64-linux-gnu'}, "'aarch64-linux-gnu'"),
        ('32-bit target', {'target': 'i686-linux-gnu'}, "'i686-linux-gnu'"),
        ('x86_64 lookalike', {'target': 'x86_64v3-linux-gnu'}, "'x86_64v3-linux-gnu'"),
        ('empty target', {'target': ''}, "targets ''"),
    )

    def test_wrong_or_malformed_compiler_answers_are_refused_for_either_driver(self):
        for label, bad, detail in self.REFUSALS:
            for broken in ('C', 'CXX'):
                with self.subTest(case=label, driver=broken):
                    host = FakeHost(self.fresh())
                    for c_name, cxx_name in helper.COMPILER_PAIRS:
                        host.compiler(c_name, **(bad if broken == 'C' else {}))
                        host.compiler(cxx_name, **(bad if broken == 'CXX' else {}))
                    with host.installed(), self.assertRaises(helper.ToolchainError) as caught:
                        helper.select_toolchain()
                    message = str(caught.exception)
                    self.assertIn('native GCC/G++ 13.3.0 toolchain is unavailable', message)
                    self.assertIn(detail, message)
                    for c_name, cxx_name in helper.COMPILER_PAIRS:
                        self.assertIn(f'{c_name}/{cxx_name}', message)

    def test_native_x86_target_spellings_are_accepted(self):
        for target in ('x86_64-linux-gnu', 'x86_64-pc-linux-gnu', 'x86_64'):
            with self.subTest(target=target):
                host = FakeHost(self.fresh())
                c_name, cxx_name = helper.COMPILER_PAIRS[0]
                host.compiler(c_name, target=target)
                host.compiler(cxx_name, target=target)
                with host.installed():
                    toolchain = helper.select_toolchain()
                self.assertEqual((toolchain.c.target, toolchain.cxx.target), (target, target))

    def test_a_pair_whose_drivers_target_different_triples_is_refused(self):
        host = FakeHost(self.fresh())
        for c_name, cxx_name in helper.COMPILER_PAIRS:
            host.compiler(c_name, target='x86_64-linux-gnu')
            host.compiler(cxx_name, target='x86_64-pc-linux-gnu')
        with host.installed(), self.assertRaises(helper.ToolchainError) as caught:
            helper.select_toolchain()
        self.assertIn('target mismatch', str(caught.exception))

    def test_a_compiler_query_that_fails_or_cannot_run_is_refused_with_its_diagnostic(self):
        cases = (
            ('-dumpmachine', (3, '', 'boom: bad flag\n'), 'exited 3: boom: bad flag'),
            ('-dumpfullversion', (1, '', ''), 'exited 1: no diagnostic'),
            ('--version', PermissionError('denied by policy'), 'cannot execute'),
        )
        for flag, answer, detail in cases:
            with self.subTest(flag=flag):
                host = FakeHost(self.fresh())
                for c_name, cxx_name in helper.COMPILER_PAIRS:
                    path = host.compiler(c_name)
                    host.compiler(cxx_name)
                    host.tools[(path, flag)] = answer
                with host.installed(), self.assertRaises(helper.ToolchainError) as caught:
                    helper.select_toolchain()
                self.assertIn(detail, str(caught.exception))

    def test_no_compiler_on_the_path_is_a_refusal_naming_every_candidate(self):
        host = FakeHost(self.fresh())
        with host.installed(), self.assertRaises(helper.ToolchainError) as caught:
            helper.select_toolchain()
        for c_name, cxx_name in helper.COMPILER_PAIRS:
            self.assertIn(f'{c_name}/{cxx_name}: missing {c_name}, {cxx_name}',
                          str(caught.exception))

    def test_a_host_that_is_not_x86_64_or_cannot_answer_is_refused_before_any_compiler_runs(self):
        cases = (
            ((0, 'aarch64\n', ''), "uname reports 'aarch64'"),
            ((0, '\n', ''), "uname reports ''"),
            ((1, '', 'uname broke\n'), 'exited 1: uname broke'),
        )
        for answer, detail in cases:
            with self.subTest(answer=answer):
                host = FakeHost(self.fresh())
                host.uname = answer
                self.install_pairs(host, ['13.3.0'] * len(helper.COMPILER_PAIRS))
                with host.installed(), self.assertRaises(helper.ToolchainError) as caught:
                    helper.select_toolchain()
                self.assertIn(detail, str(caught.exception))
                self.assertEqual([argv for argv, _ in host.calls], [['uname', '-m']])


class CacheParsingTests(TempTestCase):
    def test_entries_parse_with_types_equals_signs_and_comment_decoys(self):
        path = self.tmp / 'CMakeCache.txt'
        path.write_text('\n'.join([
            '# This is the CMakeCache file.',
            '',
            '//Choose the type of build: Debug, Release (a=b is help text, not an entry)',
            'CMAKE_BUILD_TYPE:STRING=Release',
            '# CMAKE_BUILD_TYPE:STRING=Debug',
            '//CMAKE_BUILD_TYPE:STRING=Debug',
            'CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG=1',
            'CMAKE_GENERATOR:INTERNAL=Unix Makefiles',
            'EMPTY_ENTRY:STRING=',
            'NO_TYPE=ignored',
            'NO_EQUALS:STRING',
            '']), encoding='utf-8')
        values = helper._cache_values(path)
        self.assertEqual(values['CMAKE_BUILD_TYPE'], 'Release')
        self.assertEqual(values['CMAKE_CXX_FLAGS_RELEASE'], '-O3 -DNDEBUG=1')
        self.assertEqual(values['CMAKE_GENERATOR'], 'Unix Makefiles')
        self.assertEqual(values['EMPTY_ENTRY'], '')
        self.assertNotIn('NO_TYPE', values)
        self.assertNotIn('NO_EQUALS', values)

    def test_a_missing_cache_reads_as_no_entries(self):
        self.assertEqual(helper._cache_values(self.tmp / 'absent' / 'CMakeCache.txt'), {})


class ConfigureVerificationTests(TempTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.cc = str(self.tmp / 'bin' / 'gcc-13.3.0')
        self.cxx = str(self.tmp / 'bin' / 'g++-13.3.0')
        self.toolchain = toolchain_at(self.cc, self.cxx)

    def verify(self, cache, descriptions) -> list[str]:
        build = self.fresh()
        write_cmake_tree(build, cache, descriptions)
        return helper._verify_configure(build, self.toolchain)

    def test_a_configure_that_is_exactly_as_required_has_no_findings(self):
        self.assertEqual(self.verify(*good_tree(self.cc, self.cxx)), [])

    def test_every_cache_entry_is_checked_against_its_own_requirement(self):
        wrong = {'CMAKE_C_COMPILER': '/usr/bin/cc', 'CMAKE_CXX_COMPILER': '/usr/bin/c++',
                 'CMAKE_GENERATOR': 'Ninja', 'CMAKE_BUILD_TYPE': 'Debug',
                 'CMAKE_CXX_STANDARD': '20'}
        for key, value in wrong.items():
            with self.subTest(key=key, change='wrong value'):
                cache, descriptions = good_tree(self.cc, self.cxx)
                cache[key] = value
                findings = self.verify(cache, descriptions)
                self.assertEqual(len(findings), 1, findings)
                self.assertIn(key, findings[0])
                self.assertIn(repr(value), findings[0])
            with self.subTest(key=key, change='absent'):
                cache, descriptions = good_tree(self.cc, self.cxx)
                del cache[key]
                findings = self.verify(cache, descriptions)
                self.assertEqual(len(findings), 1, findings)
                self.assertIn(f'{key} expected', findings[0])
                self.assertIn('got None', findings[0])

    def test_every_detected_compiler_setting_is_checked_for_each_language(self):
        for language in ('C', 'CXX'):
            for suffix, value in (('', '/usr/bin/cc'), ('_ID', 'Clang'), ('_VERSION', '13.2.0')):
                with self.subTest(language=language, setting=suffix):
                    cache, descriptions = good_tree(self.cc, self.cxx)
                    descriptions[language][f'CMAKE_{language}_COMPILER{suffix}'] = value
                    findings = self.verify(cache, descriptions)
                    self.assertEqual(len(findings), 1, findings)
                    self.assertIn(f'detected CMAKE_{language}_COMPILER{suffix} ', findings[0])
                    self.assertIn(repr(value), findings[0])

    def test_a_missing_compiler_description_is_a_finding_not_a_pass(self):
        for language in ('C', 'CXX'):
            with self.subTest(language=language):
                cache, descriptions = good_tree(self.cc, self.cxx)
                del descriptions[language]
                findings = self.verify(cache, descriptions)
                self.assertEqual(len(findings), 1, findings)
                self.assertIn(f'no CMake {language} compiler description', findings[0])

    def test_a_stale_description_from_another_cmake_version_cannot_hide_a_wrong_compiler(self):
        cache, descriptions = good_tree(self.cc, self.cxx)
        build = self.fresh()
        write_cmake_tree(build, cache, descriptions, version_dir='3.30.1')
        write_description(build, 'C', {
            'CMAKE_C_COMPILER': self.cc, 'CMAKE_C_COMPILER_ID': 'GNU',
            'CMAKE_C_COMPILER_VERSION': '12.4.0'}, version_dir='3.28.3')
        findings = helper._verify_configure(build, self.toolchain)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("'12.4.0'", findings[0])

    def test_an_unconfigured_tree_fails_on_every_requirement(self):
        findings = helper._verify_configure(self.fresh(), self.toolchain)
        for key in ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'CMAKE_GENERATOR',
                    'CMAKE_BUILD_TYPE', 'CMAKE_CXX_STANDARD'):
            self.assertTrue(any(finding.startswith(f'{key} expected') for finding in findings),
                            (key, findings))
        for language in ('C', 'CXX'):
            self.assertTrue(any(f'no CMake {language} compiler description' in finding
                                for finding in findings), (language, findings))


class ConfigureDefinitionTests(TempTestCase):
    def test_compiler_flags_follow_the_selected_toolchain_not_a_literal(self):
        for cc, cxx in (('/opt/a/gcc', '/opt/a/g++'),
                        ('/usr/local/bin/gcc-13.3.0', '/usr/local/bin/g++-13.3.0')):
            with self.subTest(cc=cc):
                flags = helper._cmake_definitions(toolchain_at(cc, cxx))
                self.assertIn(f'-DCMAKE_C_COMPILER={cc}', flags)
                self.assertIn(f'-DCMAKE_CXX_COMPILER={cxx}', flags)
                self.assertEqual(sorted(flags), sorted(expected_definitions(cc, cxx)))

    def test_full_release_source_layer_build_with_every_optional_target_off(self):
        flags = helper._cmake_definitions(toolchain_at('/opt/gcc', '/opt/g++'))
        values = dict(flag[2:].split('=', 1) for flag in flags)
        self.assertEqual(len(values), len(flags), 'a flag is repeated')
        self.assertEqual(values['CMAKE_BUILD_TYPE'], 'Release')
        self.assertEqual(values['CMAKE_EXPORT_COMPILE_COMMANDS'], 'ON')
        self.assertEqual(values['PINEFORGE_VERSION_SOURCE'], 'FILE')
        self.assertEqual(values['PINEFORGE_BUILD_SOURCE_LAYER'], 'ON')
        for option in ('TESTS', 'EXAMPLES', 'CORPUS_STRATEGIES', 'TUTORIAL', 'LIVE_RUNNER',
                       'BENCH_STRATEGIES', 'SPEED_BENCH'):
            self.assertEqual(values[f'PINEFORGE_BUILD_{option}'], 'OFF', option)
        for option in ('ENABLE_COVERAGE', 'ENABLE_SANITIZERS', 'STRICT_WARNINGS',
                       'REQUIRE_ABI_RECEIPTS'):
            self.assertEqual(values[f'PINEFORGE_{option}'], 'OFF', option)

    def test_every_project_flag_names_an_option_the_repository_declares(self):
        sources = [ROOT / 'CMakeLists.txt', *sorted((ROOT / 'cmake').glob('*.cmake'))]
        text = '\n'.join(path.read_text(encoding='utf-8', errors='replace')
                         for path in sources)
        names = [flag[2:].split('=', 1)[0]
                 for flag in helper._cmake_definitions(toolchain_at('/opt/gcc', '/opt/g++'))
                 if flag.startswith('-DPINEFORGE_')]
        self.assertGreaterEqual(len(names), 12)
        for name in names:
            with self.subTest(option=name):
                self.assertRegex(text, r'\b' + re.escape(name) + r'\b')


class ArgumentTests(unittest.TestCase):
    BASE = {'--source': 'src', '--build-dir': 'bld', '--artifact-dir': 'art', '--jobs': '4'}

    def parse(self, overrides=None, drop=(), extra=()):
        values = {**self.BASE, **(overrides or {})}
        argv = [item for key, value in values.items() if key not in drop
                for item in (key, value)]
        return helper.parse_args([*argv, *extra])

    def refused(self, **kwargs) -> int:
        with contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as caught:
            self.parse(**kwargs)
        return caught.exception.code

    def test_every_input_is_consumed_into_the_parsed_arguments(self):
        for jobs in ('1', '4', '64'):
            with self.subTest(jobs=jobs):
                args = self.parse({'--jobs': jobs})
                self.assertEqual(args.jobs, int(jobs))
                self.assertEqual((args.source, args.build_dir, args.artifact_dir),
                                 (Path('src'), Path('bld'), Path('art')))

    def test_a_jobs_value_that_is_not_a_positive_integer_is_refused(self):
        for bad in ('0', '-4', 'many', '2.5', ''):
            with self.subTest(jobs=bad):
                self.assertEqual(self.refused(overrides={'--jobs': bad}), 2)

    def test_every_input_is_required(self):
        for flag in self.BASE:
            with self.subTest(missing=flag):
                self.assertEqual(self.refused(drop=(flag,)), 2)

    def test_the_helper_offers_no_target_install_or_compiler_choice(self):
        for extra in (('--target', 'all'), ('--install',), ('--compiler', 'gcc-12'),
                      ('--cc', 'clang')):
            with self.subTest(extra=extra):
                self.assertEqual(self.refused(extra=extra), 2)


class HelperRunTests(TempTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.runs = 0
        self.reset_host()

    def reset_host(self, *, version: str = '13.3.0') -> None:
        self.host = FakeHost(self.tmp)
        for index, (c_name, cxx_name) in enumerate(helper.COMPILER_PAIRS):
            c_path = self.host.compiler(c_name, version=version)
            cxx_path = self.host.compiler(cxx_name, version=version)
            if index == 0:
                self.cc, self.cxx = c_path, cxx_path

    def run_helper(self, *, jobs: int = 7, patches=()) -> Outcome:
        self.runs += 1
        base = self.tmp / f'run-{self.runs}'
        source = base / 'checkout'
        source.mkdir(parents=True)
        build = base / 'build-x86-gcc133'
        artifacts = base / 'ci-native-x86-gcc133'
        argv = ['--source', str(source), '--build-dir', str(build),
                '--artifact-dir', str(artifacts), '--jobs', str(jobs)]
        out, err = io.StringIO(), io.StringIO()
        self.host.calls.clear()
        with contextlib.ExitStack() as stack:
            stack.enter_context(self.host.installed())
            for patch in patches:
                stack.enter_context(patch)
            stack.enter_context(contextlib.redirect_stdout(out))
            stack.enter_context(contextlib.redirect_stderr(err))
            code = helper.main(argv)
        return Outcome(code, out.getvalue(), err.getvalue(), source, build, artifacts)

    def cmake_calls(self) -> list[tuple[list[str], dict]]:
        return [(argv, kwargs) for argv, kwargs in self.host.calls if argv[0] == 'cmake']

    def test_success_issues_exactly_the_configure_and_build_argv_in_order(self):
        for jobs in (3, 17):
            with self.subTest(jobs=jobs):
                self.reset_host()
                outcome = self.run_helper(jobs=jobs)
                self.assertEqual(outcome.code, 0, outcome.err)
                cmake = self.cmake_calls()
                self.assertEqual(len(cmake), 2, [argv for argv, _ in cmake])
                (configure, configure_kwargs), (build, build_kwargs) = cmake
                self.assertEqual(configure[:7], ['cmake', '-S', str(outcome.source), '-B',
                                                 str(outcome.build), '-G', 'Unix Makefiles'])
                self.assertEqual(sorted(configure[7:]),
                                 sorted(expected_definitions(self.cc, self.cxx)))
                self.assertEqual(build, ['cmake', '--build', str(outcome.build), '--parallel',
                                         str(jobs), '--', '-k', 'VERBOSE=1'])
                for argv in (configure, build):
                    for refused in ('--target', '-t', '--install', '--preset', 'ctest'):
                        self.assertNotIn(refused, argv)
                for kwargs in (configure_kwargs, build_kwargs):
                    self.assertEqual(kwargs['cwd'], outcome.source)
                    self.assertEqual(kwargs['env']['CC'], self.cc)
                    self.assertEqual(kwargs['env']['CXX'], self.cxx)
                argvs = [argv for argv, _ in self.host.calls]
                queried = [index for index, argv in enumerate(argvs)
                           if argv[0] in (self.cc, self.cxx)]
                self.assertEqual(len(queried), 6)
                self.assertLess(max(queried), argvs.index(configure))
                self.assertLess(argvs.index(configure), argvs.index(build))

    def test_success_retains_the_argv_logs_identity_and_build_evidence(self):
        outcome = self.run_helper(jobs=5)
        self.assertEqual(outcome.code, 0, outcome.err)
        (configure, _), (build, _) = self.cmake_calls()
        self.assertEqual(outcome.read('configure.argv'), '\n'.join(configure) + '\n')
        self.assertEqual(outcome.read('build.argv'), '\n'.join(build) + '\n')
        self.assertEqual(outcome.read('configure.exitcode'), '0\n')
        self.assertEqual(outcome.read('build.exitcode'), '0\n')
        self.assertEqual(outcome.read('configure.stdout'), 'configure stdout line\n')
        self.assertEqual(outcome.read('configure.stderr'), 'configure stderr line\n')
        self.assertEqual(outcome.read('build.stdout'), 'build stdout line\n')
        self.assertEqual(outcome.read('build.stderr'), 'build stderr line\n')
        self.assertIn('configure stdout line', outcome.out)
        self.assertIn('build stdout line', outcome.out)
        self.assertIn('configure stderr line', outcome.err)
        self.assertIn('build stderr line', outcome.err)
        identity = outcome.read('compiler-identity.txt')
        for expected in ('uname_machine=x86_64', f'C_compiler={self.cc}',
                         f'CXX_compiler={self.cxx}', 'C_compiler_version=13.3.0',
                         'CXX_compiler_target=x86_64-linux-gnu', GOOD_BANNER):
            self.assertIn(expected, identity)
        self.assertNotIn('selection_error', identity)
        source_identity = outcome.read('source-identity.txt')
        for expected in (f'source={outcome.source}', 'git_head_exit=0', HEAD,
                         'git_status_exit=0', 'M scripts/example.py'):
            self.assertIn(expected, source_identity)
        self.assertEqual(outcome.read('CMakeCache.txt'),
                         (outcome.build / 'CMakeCache.txt').read_text(encoding='utf-8'))
        commands = json.loads(outcome.read('compile_commands.json'))
        self.assertEqual(commands[0]['command'].split()[0], self.cxx)
        for name in ('artifact.error', 'helper.error', 'configure.verification'):
            self.assertFalse(outcome.has(name), name)

    def test_a_nonzero_configure_is_returned_and_the_build_never_starts(self):
        for code in (2, 77, 139):
            with self.subTest(code=code):
                self.reset_host()
                self.host.configure_exit = code
                outcome = self.run_helper()
                self.assertEqual(outcome.code, code)
                self.assertEqual(outcome.read('configure.exitcode'), f'{code}\n')
                self.assertEqual(outcome.read('build.exitcode'), 'not-run\n')
                self.assertEqual(len(self.cmake_calls()), 1)
                self.assertIn('configure stderr line', outcome.err)
                # No tree was written, so the evidence is reported missing, and
                # that report never replaces the child's own code.
                self.assertIn('CMakeCache.txt', outcome.read('artifact.error'))

    def test_a_nonzero_build_is_returned_exactly_and_its_logs_survive(self):
        for code in (2, 9, 130):
            with self.subTest(code=code):
                self.reset_host()
                self.host.build_exit = code
                outcome = self.run_helper()
                self.assertEqual(outcome.code, code)
                self.assertEqual(outcome.read('configure.exitcode'), '0\n')
                self.assertEqual(outcome.read('build.exitcode'), f'{code}\n')
                self.assertEqual(outcome.read('build.stdout'), 'build stdout line\n')
                self.assertIn('build stdout line', outcome.out)
                self.assertFalse(outcome.has('artifact.error'))

    def test_evidence_collection_failure_never_replaces_a_prior_build_error(self):
        for code in (2, 9):
            with self.subTest(code=code):
                self.reset_host()
                self.host.build_exit = code
                failure = mock.patch.object(helper.shutil, 'copy2',
                                            side_effect=OSError('disk full'))
                outcome = self.run_helper(patches=[failure])
                self.assertEqual(outcome.code, code)
                self.assertEqual(outcome.read('build.exitcode'), f'{code}\n')
                self.assertIn('disk full', outcome.read('artifact.error'))
                self.assertIn('artifact retention failure', outcome.err)

    def test_evidence_collection_failure_fails_an_otherwise_successful_build(self):
        failure = mock.patch.object(helper.shutil, 'copy2', side_effect=OSError('disk full'))
        outcome = self.run_helper(patches=[failure])
        self.assertEqual(outcome.code, 1)
        self.assertEqual(outcome.read('build.exitcode'), '0\n')
        self.assertIn('disk full', outcome.read('artifact.error'))
        self.reset_host()
        self.host.write_compile_commands = False
        outcome = self.run_helper()
        self.assertEqual(outcome.code, 1)
        self.assertEqual(outcome.read('build.exitcode'), '0\n')
        self.assertIn('compile_commands.json', outcome.read('artifact.error'))
        self.assertTrue(outcome.has('CMakeCache.txt'))

    def test_a_configure_that_is_not_the_required_build_stops_before_the_build(self):
        cases = (
            ('generator', lambda: self.host.cache_overrides.update(CMAKE_GENERATOR='Ninja'),
             "CMAKE_GENERATOR expected 'Unix Makefiles', got 'Ninja'"),
            ('build type', lambda: self.host.cache_overrides.update(CMAKE_BUILD_TYPE='Debug'),
             "CMAKE_BUILD_TYPE expected 'Release', got 'Debug'"),
            ('standard', lambda: self.host.cache_overrides.update(CMAKE_CXX_STANDARD=None),
             "CMAKE_CXX_STANDARD expected '17', got None"),
            ('c compiler id',
             lambda: self.host.detected.update({self.cc: ('Clang', '13.3.0')}),
             "detected CMAKE_C_COMPILER_ID expected 'GNU', got 'Clang'"),
            ('cxx compiler version',
             lambda: self.host.detected.update({self.cxx: ('GNU', '13.2.0')}),
             "detected CMAKE_CXX_COMPILER_VERSION expected '13.3.0', got '13.2.0'"),
        )
        for label, change, detail in cases:
            with self.subTest(case=label):
                self.reset_host()
                change()
                outcome = self.run_helper()
                self.assertEqual(outcome.code, 1)
                self.assertIn(detail, outcome.read('configure.verification'))
                self.assertIn('configure verification failed', outcome.err)
                self.assertEqual(outcome.read('configure.exitcode'), '0\n')
                self.assertEqual(outcome.read('build.exitcode'), 'not-run\n')
                self.assertEqual(len(self.cmake_calls()), 1)
                self.assertTrue(outcome.has('CMakeCache.txt'))

    def test_a_wrong_compiler_is_refused_before_cmake_is_ever_run(self):
        for version in ('12.4.0', '13.3.1', ''):
            with self.subTest(version=version):
                self.reset_host(version=version)
                outcome = self.run_helper()
                self.assertEqual(outcome.code, 1)
                self.assertEqual(self.cmake_calls(), [])
                identity = outcome.read('compiler-identity.txt')
                self.assertIn('selection_error:', identity)
                self.assertIn(f'reports GCC {version!r}', identity)
                self.assertEqual(outcome.read('configure.exitcode'), 'not-run\n')
                self.assertEqual(outcome.read('build.exitcode'), 'not-run\n')
                self.assertIn('native x86 GNU 13.3.0 refusal', outcome.err)
                self.assertTrue(outcome.has('source-identity.txt'))

    def test_an_unanswerable_host_is_refused_and_recorded_as_unavailable(self):
        self.host.uname = (1, '', 'uname broke\n')
        outcome = self.run_helper()
        self.assertEqual(outcome.code, 1)
        identity = outcome.read('compiler-identity.txt')
        self.assertIn('uname_machine=unavailable', identity)
        self.assertIn('exited 1: uname broke', identity)
        self.assertEqual(self.cmake_calls(), [])

    def test_a_cmake_that_cannot_launch_is_a_helper_failure_not_a_pass(self):
        for phase in ('configure', 'build'):
            with self.subTest(phase=phase):
                self.reset_host()
                self.host.launch_error[phase] = FileNotFoundError(
                    2, 'No such file or directory', 'cmake')
                outcome = self.run_helper()
                self.assertEqual(outcome.code, 1)
                self.assertIn('native x86 GNU 13.3.0 helper failure',
                              outcome.read('helper.error'))
                self.assertIn('native x86 GNU 13.3.0 helper failure', outcome.err)

    def test_dropping_a_required_flag_is_caught_through_the_real_cache(self):
        real = helper._cmake_definitions

        def without(prefix):
            def definitions(toolchain):
                return [flag for flag in real(toolchain) if not flag.startswith(prefix)]
            return definitions

        for prefix, detail in (
                ('-DCMAKE_BUILD_TYPE=', "CMAKE_BUILD_TYPE expected 'Release', got None"),
                ('-DCMAKE_CXX_STANDARD=', "CMAKE_CXX_STANDARD expected '17', got None")):
            with self.subTest(dropped=prefix):
                self.reset_host()
                outcome = self.run_helper(patches=[
                    mock.patch.object(helper, '_cmake_definitions', without(prefix))])
                self.assertEqual(outcome.code, 1)
                self.assertIn(detail, outcome.read('configure.verification'))
                self.assertEqual(len(self.cmake_calls()), 1)
        with self.subTest(dropped='-DCMAKE_EXPORT_COMPILE_COMMANDS='):
            self.reset_host()
            outcome = self.run_helper(patches=[mock.patch.object(
                helper, '_cmake_definitions', without('-DCMAKE_EXPORT_COMPILE_COMMANDS='))])
            self.assertEqual(outcome.code, 1)
            self.assertIn('compile_commands.json', outcome.read('artifact.error'))


if __name__ == '__main__':
    unittest.main()
