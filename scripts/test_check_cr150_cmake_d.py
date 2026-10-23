#!/usr/bin/env python3
"""Property and fenced-tail controls for the real CR-150 CMake checker.

Each case copies the checker into a temporary repository and invokes its
normal entry point. PF_CR150_CMAKE_D_CHECKER can name the previous checker
for lab fail-before evidence. Each fence refusal first checks a legal fenced
write, so the old blanket tail refusal cannot satisfy the negative controls.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import check_cr150_cmake_d
import ci_preflight


ROOT = Path(__file__).resolve().parent.parent
CHECKER = Path(os.environ.get(
    'PF_CR150_CMAKE_D_CHECKER', ROOT / 'scripts' / 'check_cr150_cmake_d.py'))
REGISTRATION = ('add_test(NAME test_lane_property COMMAND ${Python3_EXECUTABLE} '
                '-B ${PROJECT_SOURCE_DIR}/scripts/check_cr150_cmake_d.py)\n')
FIXTURES = ROOT / 'scripts' / 'fixtures' / 'cr150_cmake_d'
INCLUDE_LOOP = '''# CR-150: sorted per-lane registrations.
file(GLOB CR150_LANE_CMAKE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cr150.d/*.cmake")
list(SORT CR150_LANE_CMAKE)
foreach(cr150_lane_file IN LISTS CR150_LANE_CMAKE)
    include("${cr150_lane_file}")
endforeach()
'''


class CMakeControls(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix='cr150-cmake-properties-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        scripts = self.root / 'scripts'
        scripts.mkdir()
        self.checker = scripts / 'check_cr150_cmake_d.py'
        shutil.copyfile(CHECKER, self.checker)
        lanes = self.root / 'tests' / 'cr150.d'
        lanes.mkdir(parents=True)
        self.lane = lanes / 'property-control.cmake'
        shutil.copyfile(ROOT / 'tests' / 'cr150.d' / 'README.md', lanes / 'README.md')
        self.cmake = self.root / 'tests' / 'CMakeLists.txt'
        self.cmake.write_text(INCLUDE_LOOP)

    def run_checker(self) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, '-B', str(self.checker)],
                              cwd=self.root, capture_output=True, text=True)


class LanePropertyControls(CMakeControls):
    def run_check(self, statement: str) -> subprocess.CompletedProcess:
        self.lane.write_text(REGISTRATION + statement + '\n')
        return self.run_checker()

    def assert_refused(self, statement: str, reason: str) -> None:
        result = self.run_check(statement)
        output = result.stdout + result.stderr
        self.assertEqual(1, result.returncode, output)
        self.assertIn(f'{self.lane}:2:', output)
        self.assertIn(reason, output)

    def test_set_target_properties_is_refused(self) -> None:
        self.assert_refused(
            'set_target_properties(pineforge PROPERTIES ENABLE_EXPORTS ON)',
            'set_target_properties')

    def test_environment_is_refused(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES ENVIRONMENT "INPUT=fixture")',
            'ENVIRONMENT')

    def test_working_directory_is_refused(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES WORKING_DIRECTORY fixture)',
            'WORKING_DIRECTORY')

    def test_environment_modification_is_refused(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES '
            'ENVIRONMENT_MODIFICATION "INPUT=set:fixture")',
            'ENVIRONMENT_MODIFICATION')

    def test_unknown_property_is_refused(self) -> None:
        self.assert_refused('set_tests_properties(test_lane_property PROPERTIES FOO bar)',
                            'FOO')

    def test_all_allowed_properties_pass(self) -> None:
        for keys in (('LABELS', 'SKIP_RETURN_CODE', 'TIMEOUT', 'RUN_SERIAL', 'DEPENDS'),
                     ('labels', 'skip_return_code', 'timeout', 'run_serial', 'depends')):
            with self.subTest(keys=keys):
                result = self.run_check(
                    'set_tests_properties(test_lane_property PROPERTIES '
                    f'{keys[0]} "ENVIRONMENT WORKING_DIRECTORY; cr150" '
                    f'{keys[1]} 77 {keys[2]} 30 {keys[3]} TRUE '
                    f'{keys[4]} [=[setup; teardown]=])')
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn('CR-150 CMake lane directory is valid', result.stdout)

    def test_missing_property_pair_is_refused(self) -> None:
        self.assert_refused('set_tests_properties(test_lane_property PROPERTIES)',
                            'needs at least one key/value pair')

    def test_odd_property_count_is_refused(self) -> None:
        self.assert_refused('set_tests_properties(test_lane_property PROPERTIES LABELS fast TIMEOUT)',
                            'key/value pairs')

    def test_refused_key_after_allowed_pairs_is_caught(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES '
            'LABELS fast TIMEOUT 30 ENVIRONMENT "INPUT=fixture")', 'ENVIRONMENT')

    def test_quoted_and_bracketed_keys_cannot_hide_environment(self) -> None:
        for key in ('"environment"', '[=[Environment]=]'):
            with self.subTest(key=key):
                self.assert_refused(
                    f'set_tests_properties(test_lane_property PROPERTIES {key} "INPUT=fixture")',
                    'ENVIRONMENT')

    def test_unquoted_list_cannot_hide_environment(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES LABELS fast;ENVIRONMENT;INPUT=fixture)',
            'ENVIRONMENT')

    def test_bracketed_value_is_one_literal_argument(self) -> None:
        result = self.run_check('''set_tests_properties(test_lane_property PROPERTIES LABELS [=[
ENVIRONMENT # literal label text, with a ) and an (
set_target_properties(literal PROPERTIES FOO bar)
]=] TIMEOUT 30)''')
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('CR-150 CMake lane directory is valid', result.stdout)

    def test_unquoted_expansion_cannot_inject_property_keys(self) -> None:
        self.assert_refused(
            'set_tests_properties(test_lane_property PROPERTIES LABELS ${lane_properties})',
            'unquoted property expansion')

    def test_quoted_expansion_is_one_property_value(self) -> None:
        result = self.run_check(
            'set_tests_properties(test_lane_property PROPERTIES LABELS "${lane_properties}")')
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('CR-150 CMake lane directory is valid', result.stdout)

    def test_foreign_test_properties_are_refused_in_a_lane_file(self) -> None:
        self.cmake.write_text(
            'add_test(NAME test_main COMMAND ${Python3_EXECUTABLE} main.py)\n'
            + INCLUDE_LOOP)
        (self.lane.parent / 'other.cmake').write_text(
            'add_test(NAME test_other COMMAND ${Python3_EXECUTABLE} other.py)\n')
        result = self.run_check(
            'set_tests_properties(test_lane_property PROPERTIES TIMEOUT 30)')
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        for name in ('test_main', 'test_other'):
            for properties in ('TIMEOUT 30', 'SKIP_RETURN_CODE 1', 'LABELS "slow"'):
                with self.subTest(name=name, properties=properties):
                    self.assert_refused(
                        f'set_tests_properties({name} PROPERTIES {properties})',
                        'is not owned by')

    def test_only_receipt_skip_return_code_is_allowed(self) -> None:
        result = self.run_check(
            'set_tests_properties(test_lane_property PROPERTIES SKIP_RETURN_CODE 77)')
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        for value in ('0', '1', '-1', '"${skip_code}"'):
            with self.subTest(value=value):
                self.assert_refused(
                    f'set_tests_properties(test_lane_property PROPERTIES SKIP_RETURN_CODE {value})',
                    'SKIP_RETURN_CODE must be 77')


class FenceControls(CMakeControls):
    OWNER = 'core-cr150-owner'
    OTHER = 'core-cr150-other'

    def setUp(self) -> None:
        super().setUp()
        self.write_lane(self.OWNER, 'pineforge_add_unit_test(test_owner)\n')
        self.write_lane(
            self.OTHER,
            'pineforge_add_unit_test(test_other)\n'
            'add_test(NAME test_other_script COMMAND ${Python3_EXECUTABLE} other.py)\n')
        self.sources('test_owner', 'test_other')

    def sources(self, *names: str) -> None:
        for name in names:
            # These files satisfy the checker's source-presence lookup only;
            # the controls never configure CMake or compile fixture sources.
            (self.root / 'tests' / f'{name}.cpp').write_text(
                '// Source-presence fixture for the CMake checker.\n')

    def write_lane(self, lane: str, text: str) -> None:
        (self.lane.parent / f'{lane}.cmake').write_text(text)

    def fence(self, body: str, lane: str = OWNER) -> str:
        return (f'# --- CR-150 {lane} begin ---\n'
                f'{body}'
                f'# --- CR-150 {lane} end ---\n')

    def real_lane(self, name: str) -> None:
        self.write_lane(f'core-cr150-{name}', (FIXTURES / f'lane-{name}.cmake').read_text())
        self.sources('test_report_sink_format', 'test_report_sink_l6b1',
                     'test_report_sink_equity', 'test_report_sink_l6b3',
                     'test_group_receipt_retirement', 'test_report_sink_selected_l17a')

    def real_tail(self, name: str) -> str:
        return (FIXTURES / f'tail-{name}.cmake').read_text()

    def assert_tail_passes(self, tail: str) -> None:
        self.cmake.write_text(INCLUDE_LOOP + tail)
        result = self.run_checker()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('CR-150 CMake lane directory is valid', result.stdout)

    def assert_tail_refused(self, tail: str, reason: str, *, before: str = '') -> None:
        # This positive half makes every new negative control fail on the
        # 5be8c54e7 checker, which rejects all nonempty fenced tails.
        self.assert_tail_passes(self.fence(
            'target_compile_definitions(test_owner PRIVATE OWNER=1)\n'))
        self.cmake.write_text(before + INCLUDE_LOOP + tail)
        result = self.run_checker()
        output = result.stdout + result.stderr
        self.assertEqual(1, result.returncode, output)
        self.assertIn(str(self.cmake), output)
        self.assertIn(reason, output)

    def test_fence_before_include_loop_is_refused(self) -> None:
        self.assert_tail_refused(
            '', 'fence must follow the CR-150 include block',
            before=self.fence('target_compile_definitions(test_owner PRIVATE OWNER=1)\n'))

    def test_unfenced_statement_after_loop_is_refused(self) -> None:
        self.assert_tail_refused(
            'target_compile_definitions(test_owner PRIVATE OWNER=1)\n',
            'unfenced statement after the CR-150 include block')

    def test_begin_without_end_is_refused(self) -> None:
        self.assert_tail_refused(
            f'# --- CR-150 {self.OWNER} begin ---\n'
            'target_compile_definitions(test_owner PRIVATE OWNER=1)\n',
            'missing end fence')

    def test_mismatched_fence_names_are_refused(self) -> None:
        self.assert_tail_refused(
            f'# --- CR-150 {self.OWNER} begin ---\n'
            f'# --- CR-150 {self.OTHER} end ---\n',
            'does not match begin fence')

    def test_fence_without_lane_file_is_refused(self) -> None:
        # The supplied L17a tail also includes L6b1, which does have a lane
        # file. Do not install the empty lane-l17a input as an ownership grant.
        self.real_lane('l6b1')
        self.assert_tail_refused(self.real_tail('l17a'),
                                 'fence has no lane file tests/cr150.d/core-cr150-l17a.cmake')

    def test_foreign_target_writes_are_refused(self) -> None:
        statements = (
            'target_compile_definitions(test_other PRIVATE STOLEN=1)',
            'target_compile_options("test_other" PRIVATE -UNDEBUG)',
            'target_link_libraries([=[test_other]=] PRIVATE pineforge)',
            'target_link_options(test_other PRIVATE -rdynamic)',
            'target_include_directories(test_other PRIVATE include)',
            'target_sources(test_other PRIVATE helper.cpp)',
            'set_target_properties(test_owner test_other PROPERTIES PREFIX "")',
            'add_library(test_othertest_owner MODULE helper.cpp)\n'
            'set_target_properties(test_other#[=[separator]=]test_owner PROPERTIES PREFIX "")',
            'add_dependencies(test_other test_owner)',
        )
        for statement in statements:
            with self.subTest(statement=statement):
                self.assert_tail_refused(self.fence(statement + '\n'),
                                         "target 'test_other' is not owned by")

    def test_add_compile_options_is_refused(self) -> None:
        self.assert_tail_refused(self.fence('add_compile_options(-UNDEBUG)\n'),
                                 'statement add_compile_options() is not allowed in a fence')

    def test_include_directories_is_refused(self) -> None:
        self.assert_tail_refused(self.fence('include_directories(include)\n'),
                                 'statement include_directories() is not allowed in a fence')

    def test_link_libraries_is_refused(self) -> None:
        self.assert_tail_refused(self.fence('link_libraries(pineforge)\n'),
                                 'statement link_libraries() is not allowed in a fence')

    def test_set_cmake_variable_is_refused(self) -> None:
        self.assert_tail_refused(self.fence('set(CMAKE_CXX_FLAGS "-UNDEBUG")\n'),
                                 'statement set() is not allowed in a fence')

    def test_other_directory_and_unknown_commands_are_refused(self) -> None:
        statements = (
            ('add_compile_definitions', 'ESCAPE=1'),
            ('add_definitions', '-DESCAPE=1'),
            ('add_link_options', '-rdynamic'),
            ('link_directories', 'lib'),
            ('set_directory_properties', 'PROPERTIES COMPILE_OPTIONS -UNDEBUG'),
            ('include', 'escape.cmake'),
            ('file', 'WRITE escape.cmake data'),
            ('execute_process', 'COMMAND true'),
            ('add_subdirectory', 'other'),
            ('cmake_language', 'EVAL CODE "message(escape)"'),
        )
        for name, arguments in statements:
            with self.subTest(name=name):
                self.assert_tail_refused(
                    self.fence(f'{name}({arguments})\n'),
                    f'statement {name}() is not allowed in a fence')

    def test_duplicate_fence_is_refused(self) -> None:
        self.assert_tail_refused(self.fence('') + self.fence(''),
                                 'duplicate CR-150 fence')

    def test_nested_fences_are_refused(self) -> None:
        self.assert_tail_refused(self.fence(self.fence('', self.OTHER)),
                                 'nested CR-150 fence')

    def test_set_property_global_is_refused(self) -> None:
        self.assert_tail_refused(
            self.fence('set_property(GLOBAL PROPERTY JOB_POOLS escape=1)\n'),
            'only set_property(TEST ...) is allowed in a fence')

    def test_set_property_directory_is_refused(self) -> None:
        self.assert_tail_refused(
            self.fence('set_property(DIRECTORY PROPERTY COMPILE_OPTIONS -UNDEBUG)\n'),
            'only set_property(TEST ...) is allowed in a fence')

    def test_set_property_skip_return_code_must_be_77(self) -> None:
        registration = (
            'add_test(NAME test_fence_skip COMMAND ${Python3_EXECUTABLE} fixture.py)\n')
        for key in ('SKIP_RETURN_CODE', 'skip_return_code'):
            with self.subTest(key=key):
                self.assert_tail_passes(self.fence(
                    registration + f'set_property(TEST test_fence_skip PROPERTY {key} 77)\n'))
                for value in ('0', '1', '-1', '"${skip_code}"', '', '77 1', '"77;1"'):
                    with self.subTest(value=value):
                        self.assert_tail_refused(self.fence(
                            registration
                            + f'set_property(TEST test_fence_skip PROPERTY {key} {value})\n'),
                            'SKIP_RETURN_CODE must be 77')

    def test_add_test_cannot_claim_another_lanes_test(self) -> None:
        self.assert_tail_refused(self.fence(
            'add_test(NAME test_other_script COMMAND ${Python3_EXECUTABLE} stolen.py)\n'
            'set_property(TEST test_other_script APPEND PROPERTY ENVIRONMENT "STOLEN=1")\n'),
            "test 'test_other_script' is created by")

    def test_add_library_cannot_claim_another_lanes_target(self) -> None:
        self.assert_tail_refused(self.fence(
            'add_library(test_other MODULE stolen.cpp)\n'),
            "target 'test_other' is created by")

    def test_foreign_test_properties_are_refused(self) -> None:
        for statement in (
                'set_property(TEST test_other_script APPEND PROPERTY ENVIRONMENT "STOLEN=1")',
                'set_property(TEST test_owner;test_other PROPERTY TIMEOUT 30)',
                'set_tests_properties(test_owner test_other PROPERTIES TIMEOUT 30)'):
            with self.subTest(statement=statement):
                self.assert_tail_refused(self.fence(statement + '\n'), 'is not owned by')

    def test_fence_cannot_claim_a_target_from_the_main_file(self) -> None:
        self.assert_tail_refused(
            self.fence('add_library(existing_module MODULE stolen.cpp)\n'),
            "target 'existing_module' is created by",
            before='add_library(existing_module MODULE existing.cpp)\n')

    def test_short_fence_name_is_not_a_lane_file_stem(self) -> None:
        self.real_lane('l6b2')
        self.assert_tail_refused(self.fence('', 'l6b2'), 'l6b2.cmake')

    def test_unfenced_statement_between_or_after_fences_is_refused(self) -> None:
        write = 'target_compile_definitions(test_owner PRIVATE OWNER=1)\n'
        for tail in (self.fence('') + write + self.fence('', self.OTHER),
                     self.fence('') + write):
            with self.subTest(tail=tail):
                self.assert_tail_refused(
                    tail, 'unfenced statement after the CR-150 include block')

    def test_canonical_fast_registration_after_final_fence_is_accepted(self) -> None:
        registration = ci_preflight.FAST_TEST_REGISTRATION
        for suffix in (
                registration,
                '# The fast-row registration stays last.\n' + registration +
                '# Only a trailing comment.\n',
                registration.replace('    ', '\t')):
            with self.subTest(suffix=suffix):
                self.assert_tail_passes(self.fence('') + suffix)

    def test_statement_after_fast_registration_is_refused(self) -> None:
        tail = self.fence('') + ci_preflight.FAST_TEST_REGISTRATION
        self.assert_tail_passes(tail)
        self.assert_tail_refused(
            tail + 'set_tests_properties(test_ta PROPERTIES LABELS "cr999")\n',
            'unfenced statement after the CR-150 include block')

    def test_statement_before_fast_registration_is_refused(self) -> None:
        self.assert_tail_passes(self.fence('') + ci_preflight.FAST_TEST_REGISTRATION)
        self.assert_tail_refused(
            self.fence('') +
            'set_tests_properties(test_ta PROPERTIES SKIP_RETURN_CODE 77)\n' +
            ci_preflight.FAST_TEST_REGISTRATION,
            'unfenced statement after the CR-150 include block')

    def test_changed_fast_registration_is_refused(self) -> None:
        registration = ci_preflight.FAST_TEST_REGISTRATION
        self.assert_tail_passes(self.fence('') + registration)
        changed = registration.replace('LABELS fast)', 'LABELS tast)', 1)
        self.assertNotEqual(registration, changed)
        self.assert_tail_refused(
            self.fence('') + changed,
            'unfenced statement after the CR-150 include block')

    def test_fast_registration_constant_matches_preflight(self) -> None:
        self.assertEqual(ci_preflight.FAST_TEST_REGISTRATION,
                         check_cr150_cmake_d.FAST_TEST_REGISTRATION)

    def test_variable_and_generator_expression_write_names_are_refused(self) -> None:
        for target in ('${destination}', '"${destination}"', '$<TARGET_FILE:test_other>',
                       'test_owner;test_other'):
            with self.subTest(target=target):
                self.assert_tail_refused(
                    self.fence(f'target_compile_definitions({target} PRIVATE ESCAPE=1)\n'),
                    'literal target and a scope')

    def test_adjacent_quoted_write_names_are_refused(self) -> None:
        self.assert_tail_refused(self.fence(
            'set_target_properties("test_other""test_owner" PROPERTIES PREFIX "")\n'),
            'fence arguments need whitespace separation')

    def test_bracketed_test_name_keeps_its_creator(self) -> None:
        self.write_lane(
            self.OTHER,
            'add_test(NAME [=[\ntest_other_script]=] COMMAND ${Python3_EXECUTABLE} other.py)\n')
        self.assert_tail_refused(self.fence(
            'add_test(NAME test_other_script COMMAND ${Python3_EXECUTABLE} stolen.py)\n'),
            "test 'test_other_script' is created by")

    def test_other_target_generator_expressions_are_refused(self) -> None:
        for expression in ('$<TARGET_PROPERTY:test_other,TYPE>',
                           '$<TARGET_LINKER_FILE:test_other>',
                           '$<${target_reader}:${target_name}>',
                           '$<TARGET_FILE:$<IF:$<BOOL:1>,test_other,test_owner>>'):
            with self.subTest(expression=expression):
                self.assert_tail_refused(self.fence(
                    f'target_compile_definitions(test_owner PRIVATE "READ={expression}")\n'),
                    'only literal $<TARGET_FILE:...> reads are allowed')

    def test_bare_other_lane_target_read_is_refused(self) -> None:
        self.assert_tail_refused(self.fence(
            'target_link_libraries(test_owner PRIVATE test_other)\n'),
            'external target reads require $<TARGET_FILE:...> or if(TARGET ...)')

    def test_alias_and_imported_targets_are_refused(self) -> None:
        for creation in ('add_library(alias ALIAS test_other)',
                         'add_library(imported MODULE IMPORTED)',
                         'add_executable(alias ALIAS test_other)'):
            with self.subTest(creation=creation):
                self.assert_tail_refused(self.fence(creation + '\n'),
                                         'ALIAS and IMPORTED targets are not allowed')

    def test_dynamic_creation_options_cannot_hide_an_alias(self) -> None:
        for creation in ('add_library(own_alias ${creation_kind} test_other)',
                         'add_library(own_module MODULE ${creation_kind})',
                         'add_executable(own_alias ${creation_kind} test_other)'):
            with self.subTest(creation=creation):
                self.assert_tail_refused(self.fence(creation + '\n'),
                                         'target creation options must be literal')

    def test_lane_file_properties_stay_restricted_with_a_fence(self) -> None:
        tail = self.fence('target_compile_definitions(test_owner PRIVATE OWNER=1)\n')
        self.assert_tail_passes(tail)
        self.lane.write_text(
            REGISTRATION +
            'set_tests_properties(test_lane_property PROPERTIES ENVIRONMENT "ESCAPE=1")\n')
        self.cmake.write_text(INCLUDE_LOOP + tail)
        result = self.run_checker()
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn('property ENVIRONMENT is explicitly forbidden in a lane file', result.stderr)

    def test_conditional_blocks_must_balance_inside_each_fence(self) -> None:
        cases = (
            ('else()\n', 'else() without if()'),
            ('elseif(UNIX)\n', 'elseif() without if()'),
            ('endif()\n', 'endif() without if()'),
            ('if(UNIX)\n', 'missing endif()'),
            ('if(UNIX)\nelse()\nelseif(APPLE)\nendif()\n', 'elseif() after else()'),
        )
        for body, reason in cases:
            with self.subTest(body=body):
                self.assert_tail_refused(self.fence(body), reason)

    def test_include_loop_contract_is_preserved(self) -> None:
        self.assert_tail_passes(self.fence(
            'target_compile_definitions(test_owner PRIVATE OWNER=1)\n'))
        for original, replacement, reason in (
                ('CONFIGURE_DEPENDS ', '', 'does not use CONFIGURE_DEPENDS'),
                ('list(SORT CR150_LANE_CMAKE)', 'list(REVERSE CR150_LANE_CMAKE)', 'is not sorted'),
                ('IN LISTS CR150_LANE_CMAKE', 'IN ITEMS other.cmake', 'does not use CR150_LANE_CMAKE'),
                ('include("${cr150_lane_file}")', 'include("other.cmake")', 'does not include each lane file')):
            with self.subTest(replacement=replacement):
                self.cmake.write_text(INCLUDE_LOOP.replace(original, replacement) + self.fence(
                    'target_compile_definitions(test_owner PRIVATE OWNER=1)\n'))
                result = self.run_checker()
                self.assertEqual(1, result.returncode, result.stdout + result.stderr)
                self.assertIn(reason, result.stderr)

    def test_real_l6b1_block_passes(self) -> None:
        self.real_lane('l6b1')
        self.assert_tail_passes(self.real_tail('l6b1'))

    def test_real_l6b2_block_passes(self) -> None:
        self.real_lane('l6b2')
        self.assert_tail_passes(self.real_tail('l6b2'))

    def test_real_l6b3_block_passes(self) -> None:
        self.real_lane('l6b3')
        self.assert_tail_passes(self.real_tail('l6b3'))

    def test_real_l9a_block_passes(self) -> None:
        self.real_lane('l9a')
        self.real_lane('l6b1')
        self.assert_tail_passes(self.real_tail('l9a'))

    def test_real_l19b_block_passes(self) -> None:
        self.real_lane('l19b')
        self.assert_tail_passes(self.real_tail('l19b'))

    def test_real_blocks_pass_together(self) -> None:
        names = ('l6b1', 'l6b2', 'l6b3', 'l9a', 'l19b')
        for name in names:
            self.real_lane(name)
        self.assert_tail_passes(''.join(self.real_tail(name) for name in names))

    def test_own_target_and_test_statements_pass(self) -> None:
        self.assert_tail_passes(self.fence('''
add_library(own_module MODULE helper.cpp)
add_executable(own_host helper.cpp)
target_compile_definitions("test_owner" PRIVATE OWN=1)
target_compile_options([=[test_owner]=] PRIVATE -UNDEBUG)
target_link_libraries(test_owner PRIVATE own_module)
target_link_options(test_owner PRIVATE "-Wl,-rpath,$<TARGET_FILE:own_module>")
target_include_directories(test_owner PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_sources(test_owner PRIVATE helper.cpp)
set_target_properties(test_owner own_module PROPERTIES PREFIX "")
add_dependencies(test_owner own_module own_host)
add_test(NAME own_script COMMAND ${Python3_EXECUTABLE} own.py)
set_property(TEST own_script test_owner APPEND PROPERTY ENVIRONMENT "OWN=1")
set_tests_properties(own_script test_owner PROPERTIES TIMEOUT 30)
if(TARGET test_other)
    target_compile_definitions(test_owner PRIVATE "OTHER=$<TARGET_FILE:test_other>")
    target_compile_definitions(test_owner PRIVATE
        "OTHER_IF_DEBUG=$<$<CONFIG:Debug>:$<TARGET_FILE:test_other>>")
elseif(APPLE)
    target_compile_definitions(test_owner PRIVATE APPLE=1)
else()
    target_compile_definitions(test_owner PRIVATE FALLBACK=1)
endif()
'''))

    def test_ownership_in_other_fence_cannot_be_claimed(self) -> None:
        creation = self.fence('add_library(foreign_module MODULE helper.cpp)\n', self.OTHER)
        write = self.fence('target_compile_definitions(foreign_module PRIVATE STOLEN=1)\n')
        for tail in (creation + write, write + creation):
            with self.subTest(tail=tail):
                self.assert_tail_refused(tail, "target 'foreign_module' is not owned by")

    def test_fence_markers_in_literal_arguments_are_not_boundaries(self) -> None:
        for opening, closing in (('[=[', ']=]'), ('"', '"')):
            with self.subTest(opening=opening):
                self.assert_tail_passes(self.fence(
                    'set_property(TEST test_owner PROPERTY ENVIRONMENT ' + opening + '''
# --- CR-150 absent begin ---
add_compile_options(literal text)
# --- CR-150 absent end ---
''' + closing + ')\n'))

    def test_fence_markers_in_bracket_comments_are_not_boundaries(self) -> None:
        self.assert_tail_passes(self.fence('''#[=[
# --- CR-150 absent begin ---
add_compile_options(comment text)
# --- CR-150 absent end ---
]=]
target_compile_definitions(test_owner PRIVATE OWNER=1)
'''))


if __name__ == '__main__':
    unittest.main(verbosity=2)
