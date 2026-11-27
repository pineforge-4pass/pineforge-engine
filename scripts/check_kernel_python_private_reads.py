#!/usr/bin/env python3
"""Class sweep: a Python ctest row may not read a private file that is gone.

L6's S-73 two-mode round 4 (kernel profile, fixtures absent) found Python rows
that read files the private-fixture removals take out of the public tree and
failed on read_text()/open(): test_native_cpp_abi (its headers.tar),
test_native_cpp_versions, test_settlement_cpp_abi_tooling,
test_verify_corpus_metrics and the two pending_order_v1 prefix rows. Each row
now either reads no private path or has the skip path the fix round gave it:
the pinned literal ("SKIP private fixtures not present: <path>") with the
row's SKIP_RETURN_CODE 77 / SKIP_REGULAR_EXPRESSION, or unittest.SkipTest
(skipUnless) around the private read.

This sweep reads the tree, names every offender with file:line, and closes
with

    kernel python rows reading an absent private file: 0

LOUD-FAIL (shared with the other private-fixture sweeps): a construct this
sweep cannot parse is an ERROR, never a silent skip --

* an add_test() whose command runs ${Python3_EXECUTABLE} but supplies
  neither a readable script nor an inline program,
* a local module a row's script imports that the sweep cannot read,
* a row whose script text cannot be decoded.

The population is every Python ctest row registered by tests/CMakeLists.txt
(a superset of any one profile's rows, so the kernel profile cannot be missed
by a row that registers only elsewhere). A "private path read" is a path under
tests/fixtures/ or tests/oracle/ named by the script's own text -- as one
literal ("tests/fixtures/...") or as the composition the tree writes them with
("tests" / "fixtures" / "...") -- that the tree does not have. A path the tree
HAS is not a private read: the sweep never flags a read it cannot prove is
missing.
"""
from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from pathlib import Path
from typing import NamedTuple

from check_lanes_cmake_d import ARGUMENT_LITERAL
from cmake_test_includes import registration_statements

ROOT = Path(__file__).resolve().parents[1]
SKIP_LITERAL = 'SKIP private fixtures not present: '
PRIVATE_PREFIXES = ('tests/fixtures/', 'tests/oracle/')
ADD_TEST = re.compile(r'add_test\(\s*NAME\s+([A-Za-z0-9_.-]+)', re.MULTILINE)
CMAKE_ARGUMENT = re.compile(ARGUMENT_LITERAL + r'|(?:\\.|[^\s;\\])+', re.DOTALL)
CMAKE_REF = re.compile(r'\$\{([^{}]+)\}')
CMAKE_UNEXPANDED_REF = re.compile(r'(?<!\\)\$(?:\{|ENV\{|CACHE\{|<)')
PYTHON = '${Python3_EXECUTABLE}'
CMAKE_ESCAPE = {'n': '\n', 'r': '\r', 't': '\t', '\n': ''}
TEST_OPTIONS = {'WORKING_DIRECTORY', 'CONFIGURATIONS', 'COMMAND_EXPAND_LISTS'}
LITERAL_PRIVATE = re.compile(r'["\']((?:[\w.-]+/)?tests/(?:fixtures|oracle)/[\w./-]+)["\']')
COMPOSED_PRIVATE = re.compile(
    r'["\']tests["\']\s*/\s*["\'](fixtures|oracle)["\']\s*/\s*["\']([\w./-]+)["\']')
LOCAL_IMPORT = re.compile(r'^\s*(?:from\s+([A-Za-z_]\w*)\s+import|import\s+([A-Za-z_]\w*))',
                          re.MULTILINE)


class SweepError(Exception):
    """A construct the sweep cannot parse; reported and exits non-zero."""


class UnknownCommand(SweepError):
    """The executable or Python program cannot be selected from this file."""


class InlineProgram(NamedTuple):
    text: str
    line: int


def argument_text(arg: re.Match) -> str:
    """Strip CMake delimiters and decode escapes, leaving bracket contents literal."""
    text = arg['bracket_value']
    if text is not None:
        return text.removeprefix('\n')
    raw = arg[0]
    text = raw[1:-1] if raw.startswith('"') else raw
    return re.sub(r'\\(.)', lambda m: CMAKE_ESCAPE.get(m[1], m[1]), text, flags=re.DOTALL)


class Word(NamedTuple):
    text: str
    line: int
    raw: str


def words(body, line):
    for arg in CMAKE_ARGUMENT.finditer(body):
        yield Word(argument_text(arg), line + body.count('\n', 0, arg.start()) +
                   int((arg['bracket_value'] or '').startswith('\n')),
                   arg[0] if arg['bracket_value'] is None else '')


def expand(words, values):
    for word in words:
        ref = CMAKE_REF.fullmatch(word.text)
        if ref and ref[1] in values and word.raw == word.text:
            for item in values[ref[1]]:
                parts = [item.text] if r'\;' in item.raw else item.text.split(';')
                yield from (item._replace(text=part) for part in parts if part)
        elif '${' in word.raw and r'\$' not in word.raw:
            def scalar(m):
                return ';'.join(w.text for w in values[m[1]]) if m[1] in values else m[0]
            yield word._replace(text=CMAKE_REF.sub(scalar, word.text),
                                raw=CMAKE_REF.sub(scalar, word.raw))
        else:
            yield word


def after_env(argv):
    if (argv and argv[0].text in ('${CMAKE_COMMAND}', 'cmake')
            and [w.text for w in argv[1:3]] == ['-E', 'env']):
        argv = argv[3:]
        while argv and (re.match(r'[\w.-]+=', argv[0].text) or argv[0].text == '--'):
            argv = argv[1:]
    return argv


def command_values(history, argv):
    """Replay only bindings used by this command, at the point each value is used."""
    needed = set(CMAKE_REF.findall(' '.join(w.text for w in argv)))
    writes = {}
    for cmd in reversed(history):
        op = cmd.name.lower()
        if (op not in ('set', 'list', 'unset', 'string', 'file', 'math', 'execute_process')
                or not any(key in cmd.body for key in needed)):
            continue
        args = list(words(cmd.body, cmd.line))
        if op == 'list' and args and args[0].text == 'APPEND':
            op, args = 'append', args[1:]
        if args and op in ('set', 'append', 'unset') and args[0].text in needed:
            key, value = args[0].text, args[1:]
            if any(w.text in ('CACHE', 'PARENT_SCOPE') for w in value):
                op = 'unknown'
            writes[cmd.line] = [(op, key, value)]
            needed.update(CMAKE_REF.findall(' '.join(w.text for w in value)))
        elif op in ('list', 'string', 'file', 'math', 'execute_process'):
            writes[cmd.line] = [('unknown', w.text, ()) for w in args if w.text in needed]

    states, blocks = [{}], []
    for cmd in history:
        op = cmd.name.lower()
        if op in ('if', 'function', 'macro', 'foreach', 'block'):
            blocks.append((states, [], False))
            if op in ('function', 'macro', 'foreach'):
                formals = cmd.body.split()
                hidden = set(formals[:1] if op == 'foreach' else formals[1:])
                states = [{k: v for k, v in s.items() if k not in hidden} for s in states]
        if blocks and op in ('else', 'elseif'):
            before, arms, _ = blocks[-1]
            arms.extend(s for s in states if s not in arms)
            states = before
            blocks[-1] = before, arms, op == 'else'
        if op in ('if', 'elseif'):
            guard = re.fullmatch(r'\s*(\w+)\s+STREQUAL\s+"([\w.-]+)"\s*', cmd.body)
            if guard and guard[1] in needed:
                value = (Word(guard[2], cmd.line, guard[2]),)
                states = [dict(s, **{guard[1]: value}) for s in states]
        elif blocks and op in ('endif', 'endfunction', 'endmacro', 'endforeach', 'endblock'):
            before, arms, exhaustive = blocks.pop()
            if op in ('endfunction', 'endmacro', 'endblock'):
                states = before
            else:
                arms.extend(s for s in states + ([] if exhaustive else before) if s not in arms)
                states = arms
        for action, key, value in writes.get(cmd.line, ()):
            if action in ('set', 'append'):
                def assigned(s):
                    prefix = s.get(key, ()) if action == 'append' else ()
                    return dict(s, **{key: prefix + tuple(expand(value, s))})
                states = [assigned(s) for s in states]
            else:
                states = [dict(s, **{key: (Word('${' + key + '}', cmd.line, ''),)}) for s in states]
    for state in states:
        yield after_env(tuple(expand(argv, state)))


def discovered_programs(directory: Path):
    """unittest discover's default test*.py files and imported package initializers."""
    for path in sorted(directory.iterdir()):
        if (path.is_file() and fnmatch.fnmatchcase(path.name, 'test*.py')
                and path.stem.isidentifier()):
            yield path
        elif path.is_dir() and (path / '__init__.py').is_file():
            yield path / '__init__.py'
            yield from discovered_programs(path)


def python_row(cmake_path: Path, line: int, statement: str, history=()):
    """The row name and every program selected by this Python add_test()."""
    fields = list(words(statement[len('add_test('):-1], line))
    if len(fields) >= 4 and fields[0].text == 'NAME' and fields[2].text == 'COMMAND':
        argv = after_env(fields[3:])
        if (argv and re.fullmatch(r'\$\{(?:[^{}]|\$\{[^{}]*\})*\}', argv[0].text)
                and argv[0].raw in (argv[0].text, '"' + argv[0].text + '"')
                and argv[0].text not in (PYTHON, '${CMAKE_COMMAND}')):
            name, var = fields[1].text, argv[0].text
            unknown = UnknownCommand(name, f'{cmake_path}:{line}: row {name} command {var}')

            def selected(args):
                if not args:
                    raise unknown
                exe = args[0].text
                if exe == PYTHON and args[0].raw and r'\$' not in args[0].raw:
                    return python_programs(cmake_path, line, name, iter(args[1:]), unknown)
                if not exe or (exe != '${CMAKE_COMMAND}' and
                               ('$' in exe or ';' in exe or exe.startswith('-'))):
                    raise unknown
                return None

            candidates = map(selected, command_values(history, argv))
            first = next(candidates)
            if any(candidate != first for candidate in candidates):
                raise unknown
            return first
    if 'Python3_EXECUTABLE' not in statement:
        return None
    name = ADD_TEST.search(statement)
    if not name:
        raise SweepError(f'{cmake_path}:{line}: add_test() with an unreadable NAME runs '
                         f'Python3_EXECUTABLE; the sweep refuses to skip it: {statement!r}')
    args = words(statement[name.end():-1], line + statement.count('\n', 0, name.end()))
    for arg in args:
        if arg.text == PYTHON:
            return python_programs(cmake_path, line, name[1], args)
    return None


def python_programs(cmake_path, line, name, args, unknown=None):
    where = f'{cmake_path}:{line}: row {name}'

    def path_for(word: str) -> Path:
        path = word.replace('${PROJECT_SOURCE_DIR}', str(ROOT)).replace(
            '${CMAKE_CURRENT_SOURCE_DIR}', str(ROOT / 'tests'))
        if '$' in path:
            raise unknown or SweepError(f'{where} names an unexpanded path {word!r}')
        return ROOT / path

    # Stop at the script or module: a later -c belongs to that program.
    for arg in args:
        option = arg.text
        if option == '-c':
            program = next(args, None)
            text = program.text if program is not None else None
            if text is None or text in TEST_OPTIONS:
                raise SweepError(f'{where} runs Python3_EXECUTABLE '
                                 'but names no .py script and -c has no program; '
                                 'the sweep cannot read it and refuses to skip it')
            if CMAKE_UNEXPANDED_REF.search(program.raw):
                raise unknown or SweepError(f'{where} has an unexpanded '
                                            'inline program; the sweep refuses to skip it')
            return name, [InlineProgram(text, program.line)]
        if option in ('-W', '-X', '--check-hash-based-pycs'):
            next(args, None)
        elif option == '-m':
            words = []
            for item in args:
                word = item.text
                if word in TEST_OPTIONS:
                    break
                words.append(word)
            if words[:3] == ['unittest', 'discover', '-s'] and len(words) == 4:
                directory = path_for(words[3])
                if not directory.is_dir():
                    raise SweepError(f'{where} discovers {words[3]!r}, which is not a directory')
                programs = list(discovered_programs(directory))
            elif words[:1] == ['pytest']:
                programs = []
                operands = iter(words[1:])
                for word in operands:
                    if word in ('-p', '-k', '-m'):
                        if next(operands, None) is None:
                            raise SweepError(f'{where} has no argument for pytest {word}')
                    elif word.partition('::')[0].endswith('.py'):
                        programs.append(path_for(word.partition('::')[0]))
                    elif not re.fullmatch(r'-(?:[qvs]+|r[A-Za-z]+)', word):
                        raise SweepError(f'{where} has an unsupported pytest operand {word!r}')
            else:
                if unknown and any('$' in word for word in words):
                    raise unknown
                raise SweepError(f'{where} has an unsupported Python module command: {words!r}')
            if not programs:
                raise SweepError(f'{where} selects no Python programs')
            return name, programs
        elif option == '--':
            continue
        elif option == '-' or not option.startswith('-'):
            if option.endswith('.py'):
                return name, [path_for(option)]
            if unknown and '$' in option:
                raise unknown
            break
    raise SweepError(f'{where} runs Python3_EXECUTABLE but names no .py script or inline program')


def module_closure(script: Path, seen=None, *, text=None, source_dir=None):
    """The program's Python source plus every local module it imports, transitively."""
    seen = seen if seen is not None else {}
    if script in seen:
        return seen
    if text is None:
        try:
            text = script.read_text()
        except (OSError, UnicodeError) as error:
            raise SweepError(f'{script}: unreadable: {error}') from error
    seen[script] = text
    for match in LOCAL_IMPORT.finditer(text):
        name = match.group(1) or match.group(2)
        for directory in (source_dir or script.parent, ROOT / 'scripts'):
            candidate = directory / f'{name}.py'
            if candidate.is_file() and candidate not in seen:
                module_closure(candidate, seen)
    return seen


def private_reads(closure):
    """Absent tests/fixtures|oracle paths named by the closure's text."""
    missing = []
    for path, text in closure.items():
        for match in LITERAL_PRIVATE.finditer(text):
            candidate = ROOT / match.group(1)
            if not candidate.exists():
                line = text.count('\n', 0, match.start()) + 1
                missing.append((path, line, match.group(1)))
        for match in COMPOSED_PRIVATE.finditer(text):
            reference = f'tests/{match.group(1)}/{match.group(2)}'
            if not (ROOT / reference).exists():
                line = text.count('\n', 0, match.start()) + 1
                missing.append((path, line, reference))
    return missing


def row_properties(registrations):
    """Map only the names before PROPERTIES to their assigned keys and values."""
    found = {}
    for source, cmd in registrations:
        if cmd.name.lower() != 'set_tests_properties':
            continue
        words = [argument_text(arg) for arg in CMAKE_ARGUMENT.finditer(cmd.body)]
        if 'PROPERTIES' not in words:
            raise SweepError(f'{source}:{cmd.line}: missing PROPERTIES')
        at = words.index('PROPERTIES')
        if not at or at + 1 == len(words) or (len(words) - at - 1) % 2:
            raise SweepError(f'{source}:{cmd.line}: incomplete set_tests_properties')
        for row in words[:at]:
            found.setdefault(row, {}).update(zip(words[at + 1::2], words[at + 2::2]))
    return found


# Rows whose closure names an absent private path that the row provably never
# reads from the real tree. Each entry is printed on every run, with the
# evidence that earned it, so an exemption is never silent. Regaining the
# exemption requires re-running its evidence: scripts/test_private_fixture_
# reads.py (ExemptRowTraceControls) re-runs it in-tree for every row this
# table exempts -- it traces each row's script, fails when an open lands under
# the real tests/fixtures/ or tests/oracle/, and fails a tampered copy that
# does read, so a name on this table without a clean trace does not pass.
EXEMPT = {
    'test_private_fixture_guards': (
        'the paths it names are temp-world payloads (PROBES), not reads: under '
        'a sys.addaudithook(open) trace it opens 0 files under the real '
        'tests/fixtures/ and exits 0 on the fixture-less tree; the trace is '
        're-run in-tree by control 12 of scripts/test_private_fixture_reads.py'),
    'test_private_fixture_reads_controls': (
        'the path it names is a tamper payload (REMOVED_EXAMPLE, the removed '
        'directory the controls copy into their temp trees), not a read: under '
        'a sys.addaudithook(open) trace the script opens 0 files under the '
        'real tests/fixtures/ and tests/oracle/, in its own process and in the '
        'Python children it starts; control 12 of scripts/test_private_'
        'fixture_reads.py re-runs that trace'),
    'test_oracle_fixture_checks': (
        'the private paths it names are payloads of the synthetic roots its '
        'controls build: the suite copies tests/oracle/ (the KEPT files) into '
        'a temp root and writes the three private oracle sources there itself, '
        'so it never opens an ABSENT private file of the real tree; under the '
        'trace that records exactly those opens it comes out clean, and the '
        'three of its controls that assert the checkers’ private-fixture '
        'behaviour (the flag, the pin’s removal set) skip when the tree '
        'holds the oracles, because the overlay world is not what they '
        'assert. Control 12 of scripts/test_private_fixture_reads.py re-runs '
        'the trace'),
    'test_two_world_registration': (
        'the path it names is the overlay MARKER (OVERLAY_MARKER), not a read: '
        'the script probes it with Path.exists() -- an os.stat, which the '
        'open-audit hook does not observe -- to decide whether the overlay '
        'half runs, and every file it opens is gated on that same world '
        'existence test (load_pinned_rows, reaches_source_layer, parent_list, '
        'file(STRINGS)), so no file under the real tests/fixtures/ or '
        'tests/oracle/ is ever opened, present or absent. Without the '
        'fixtures the row exits 0 and prints its own not-run line ("overlay '
        'half not run: private fixtures not present: <marker>"); it carries '
        'no skip code. Control 12 of scripts/test_private_fixture_reads.py '
        're-runs the script under the trace with the argument of its own '
        'registration (--both) and with the script’s own directory first on '
        'sys.path, where `python <script>` puts it, so its `import '
        'ci_verify` resolves. The control fails unless the script runs to '
        'its end with exit 0 and the trace records no open of an absent '
        'file under the real tests/fixtures/ or tests/oracle/. Control 15 '
        'adds one such open to a copy of the script and fails unless the '
        'trace names it'),
    'test_private_marker_rows_registration': (
        'the two private paths it names (schedule and cross_side of the '
        'exit-binding control) are scratch-tree payloads, not reads: probe() '
        'accepts only relative removed files of the pin, and Probe writes '
        'each as a stand-in under its own tempfile.TemporaryDirectory tree, '
        'where the block cut from tests/CMakeLists.txt asks only whether the '
        'file exists. In the real tree the script opens three files, none '
        'under tests/fixtures/ or tests/oracle/: tests/CMakeLists.txt, '
        'tests/private_fixture_paths.txt and scripts/check_two_world_'
        'registration.py (for OVERLAY_MARKER, read from the text, never '
        'probed). It starts no Python child: its children are cmake and '
        'ctest on the scratch project, whose rows are cmake -E echo '
        'commands, and those two are outside an in-process audit hook. It '
        'has no skip path: on the fixture-less tree it runs every test and '
        'exits 0. Control 12 of scripts/test_private_fixture_reads.py '
        're-runs the script under the trace with the arguments of its own '
        'registration, which passes five CMake values (cmake, ctest, the '
        'generator, the make program and a work directory under the build '
        'tree): the ctest row of the controls hands over its build tree’s '
        'values for those five names, and a run of the controls that was '
        'not given them fails this row by name instead of tracing it some '
        'other way. The control fails unless that trace records no open of '
        'an absent file under the real tests/fixtures/ or tests/oracle/ and '
        'the script exits 0. The trace covers the script’s own process; '
        'what cmake and ctest open is outside it and follows from their '
        'command lines (a scratch source tree and a scratch build tree), '
        'not from the hook'),
}
GATED_BY_MODULE = re.compile(r'(?:return|Exit|exit)\s*\(?\s*77\b')


def module_gates_its_own_paths(text: str) -> bool:
    """The module that names the path also carries the pinned literal and a
    77 exit -- the settlement-ABI family's own gate (95c27eaff)."""
    return SKIP_LITERAL in text and GATED_BY_MODULE.search(text) is not None


def has_skip_path(properties, closure) -> bool:
    """The row's own skip path: the pinned literal plus a ctest skip property,
    or a unittest SkipTest/skipUnless around the private read."""
    texts = closure.values()
    if not any(SKIP_LITERAL in text for text in texts):
        return False
    if properties.get('SKIP_RETURN_CODE') == '77':
        return True
    if 'private fixtures not present' in properties.get('SKIP_REGULAR_EXPRESSION', ''):
        return True
    return any(('SkipTest' in text or 'skipUnless' in text) for text in texts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', type=Path, default=ROOT / 'tests' / 'CMakeLists.txt')
    args = parser.parse_args()
    cmake_path = args.cmake
    offenders = []
    errors = []
    unknown_command = False
    history = {}
    programs_seen = 0
    try:
        registrations = list(registration_statements(cmake_path))
        properties = row_properties(registrations)
    except (ValueError, SweepError) as error:
        print(f'check_kernel_python_private_reads: NOT COMPARED: {error}', file=sys.stderr)
        return 2
    for source, cmd in registrations:
        previous = history.setdefault(source, [])
        if cmd.name.lower() != 'add_test':
            previous.append(cmd)
            continue
        try:
            row = python_row(source, cmd.line, f'add_test({cmd.body})', previous)
            if row is None:
                continue
            name, programs = row
            for program in programs:
                try:
                    if isinstance(program, InlineProgram):
                        closure = module_closure(source, text=program.text, source_dir=cmake_path.parent)
                    else:
                        closure = module_closure(program)
                except SweepError as error:
                    errors.append(f'{source}:{cmd.line}: row {name}: {error}')
                    continue
                programs_seen += 1
                unguarded = [read for read in private_reads(closure)
                             if read[0] == source or not module_gates_its_own_paths(closure[read[0]])]
                if (unguarded and name not in EXEMPT
                        and not has_skip_path(properties.get(name, {}), closure)):
                    path, line, reference = unguarded[0]
                    if path == source:
                        line += program.line - 1
                    offenders.append((name, (path, line, reference)))
        except UnknownCommand as error:
            name, message = error.args
            print(f'NOT COMPARED: {message}', file=sys.stderr)
            unknown_command |= '${' not in name
        except (OSError, SweepError) as error:
            errors.append(str(error))
    for name, (path, line, reference) in offenders:
        print(f'{name}: reads absent {reference} ({path}:{line}) '
              f'without printing {SKIP_LITERAL!r} and skipping')
    for name, reason in sorted(EXEMPT.items()):
        print(f'exempt: {name} -- {reason}')
    if not programs_seen and not errors:
        errors.append('no registered Python programs')
    if errors or unknown_command:
        for error in errors:
            print(f'check_kernel_python_private_reads: NOT COMPARED: {error}', file=sys.stderr)
        return 2
    count = len({name for name, _ in offenders})
    print(f'kernel python rows reading an absent private file: {count}')
    return 1 if offenders else 0


if __name__ == '__main__':
    raise SystemExit(main())
