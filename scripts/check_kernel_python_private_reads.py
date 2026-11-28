#!/usr/bin/env python3
"""Class sweep: a Python ctest row may not read a private file that is gone.

L6's S-73 two-mode round 4 (kernel profile, fixtures absent) found Python rows
that read files the private-fixture removals take out of the public tree and
failed on read_text()/open(): test_native_cpp_abi (its headers.tar),
test_native_cpp_versions, test_settlement_cpp_abi_tooling,
test_verify_corpus_metrics and the two pending_order_v1 prefix rows. Each row
now either reads no private path or has the skip path the fix round gave it:
the pinned literal ("SKIP private fixtures not present: <path>") with the
row's SKIP_RETURN_CODE 77 / SKIP_REGULAR_EXPRESSION, or with a unittest skip
(SkipTest, skipUnless) that can run in the module that names the path. Whether
that skip stands around the read is not checked.

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
quoted literal that starts with tests/fixtures/, or as the composition the tree
writes them with ("tests" / "fixtures" / <name>) -- that the tree does not have.
This docstring spells neither form in full, so the sweep's own text names no
private path. A path the tree HAS is not a private read: the sweep never flags
a read it cannot prove is missing.
"""
from __future__ import annotations

import argparse
import ast
import fnmatch
import functools
import re
import sys
import warnings
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
        'fixture_reads.py re-runs that trace; control 16 of that script names '
        'the two stand-in paths of test_private_marker_rows_registration '
        '(schedule and cross_side of the exit-binding control) as substring '
        'needles, looked for in the text of the script of that row and '
        'never opened'),
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
# ---- a module's own gate, read from its syntax tree -------------------------
# A module gates a private path it names when a guard that can run stands
# before the read. The module is parsed with ast: a guard spelled inside a
# string constant or a comment is not a statement and gates nothing.
#
# The guards (guard_form): `return 77`, `raise SystemExit(77)`, a call
# statement that really exits with 77, and `raise SkipTest` (also as
# unittest.SkipTest(...)). The 77 is the literal: a name bound to 77 is not
# read. A call really exits (really_exits) when it is `exit(77)` or `quit(77)`
# and the module binds no such name anywhere, or `sys.exit(77)` /
# `os._exit(77)` on a name that nothing but a plain import of that module
# binds (`import sys as system` counts, `import os as sys` does not).
# `skipUnless` gates as a decorator, on the function that names the path or on
# a class or function around that function, written as a call or as a
# module-level name bound to such a call.
#
# Reachable (first_guard): the guard is a statement of the scope's own flow,
# at any depth under if / else / for / while / with / try / class. It does not
# count behind a statement that always leaves its block (return, raise, break,
# continue, a call that really exits, an `if` whose two branches both leave),
# in a branch a constant test rules out (`if False:`, the else of `if True:`,
# `while 0:`), in the body of another def, or, for a line outside it, in an
# `if` on `__name__`: a main block does not run when a test runner imports
# the module.
#
# Guards the read (read_gate), by the line that names the path:
#   1. order, for a line that opens the path where it names it: the guard is
#      in an earlier statement of the block that holds the line, or of a
#      block around it, in the function that names the path or in a scope
#      around that function (a guard at module top level gates what the
#      module defines after it). A guard behind the read does not count, nor
#      one in a sibling branch. The test of the `if` whose branches hold the
#      guard is the probe, and a path named there is gated. What that test
#      asks is not read: the line has no name to look for in it;
#   2. binding: where the line only binds the path to a name (an assignment
#      whose value builds the path from operators, containers and a path
#      builder such as Path(...)), nothing is opened there and the read is
#      wherever the name goes. Order alone decides nothing here: a guard
#      above the binding is judged like any other. The one bound name is
#      followed through its scope (bound_gate), with every name an assignment
#      of that scope only passes it along to. A guard gates the path when all
#      three hold:
#        a. it can run: it is in the scope that binds the path, or, for a
#           module-level binding, in a module-level function the top level
#           calls by name -- the settlement-ABI form (95c27eaff), `return 77`
#           in main() under `raise SystemExit(main())`;
#        b. it is a statement of the body or the else-branch of an `if`, and
#           that `if` is an existence test of the bound path. It names the
#           path: a followed name stands in the test, in a statement before
#           the `if` that feeds the test (fed_by), or in the body of a
#           module-level function those call. And an absent path reaches the
#           guard (absent_truth): the test is true where the guard is in the
#           body, false where it is in the else-branch, read from `not`,
#           `and` / `or`, P.exists() / P.is_file() / P.is_dir(),
#           os.path.exists(P), a list of the paths that fail such a test,
#           and the one module-level function that returns one;
#        c. that `if` dominates every use of a followed name: the use is in
#           the body or the else-branch of the `if`, or inside a statement
#           that follows the `if` in the `if`'s own block. A use in the test,
#           or in what feeds the test, counts only where it opens nothing
#           (opens_nothing): feeding the test excuses no read, and the
#           variable of a `for` that feeds it is followed through the loop. A
#           use in a statement before the `if`, in a sibling branch, or
#           behind the block the `if` stands in is not dominated. A use
#           inside another function stands where that function is referred
#           to: something has to refer to it, and every reference has to be
#           dominated in turn. A function nothing refers to may still be
#           called by a test runner, so its uses are not dominated.
# For the 77 forms the module must also hold the pinned literal in a string
# constant, as before; whether the guard's own block prints it is not read.
EXIT_FUNCTIONS = {'sys': 'exit', 'os': '_exit'}
BUILTIN_EXITS = ('exit', 'quit')
EXISTENCE_METHODS = ('exists', 'is_file', 'is_dir')
EXISTENCE_FUNCTIONS = ('exists', 'isfile', 'isdir', 'lexists')
CONTAINER_VIEWS = ('values', 'items', 'keys', 'get')
PURE_BUILTINS = ('next', 'iter', 'list', 'tuple', 'set', 'frozenset', 'sorted', 'reversed',
                 'len', 'any', 'all', 'bool')
PATH_BUILDERS = ('Path', 'PurePath', 'PurePosixPath', 'joinpath', 'str')
PLAIN_HOLDERS = (ast.BinOp, ast.BoolOp, ast.UnaryOp, ast.Compare, ast.IfExp, ast.Tuple,
                 ast.List, ast.Set, ast.Dict, ast.JoinedStr, ast.FormattedValue,
                 ast.Subscript, ast.Starred)
DEFS = (ast.FunctionDef, ast.AsyncFunctionDef)


class Gate(NamedTuple):
    """Whether a module gates one private path it names, and why."""
    guarded: bool
    reason: str


def callee(call) -> str:
    """The last name of what a call calls: `exit` for sys.exit(...)."""
    func = call.func
    return func.attr if isinstance(func, ast.Attribute) else getattr(func, 'id', '')


def is_skip_code(node) -> bool:
    return isinstance(node, ast.Constant) and type(node.value) is int and node.value == 77


def bound_names(tree) -> dict:
    """Every name the module binds, anywhere in its tree, mapped to how: the
    module a plain import binds it to (`import os.path` gives os -> {'os'},
    `import sys as system` gives system -> {'sys'}), and '' for any other
    binding (a def, a class, an assignment, a parameter, a loop or `with`
    target, `from ... import`)."""
    bound = {}

    def bind(name, origin=''):
        if name:
            bound.setdefault(name, set()).add(origin)

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                top = alias.name.partition('.')[0]
                bind(alias.asname or top, alias.name if alias.asname else top)
        elif isinstance(node, ast.ImportFrom):
            for alias in node.names:
                bind(alias.asname or alias.name)
        elif isinstance(node, ast.Name):
            if not isinstance(node.ctx, ast.Load):
                bind(node.id)
        elif isinstance(node, ast.arg):
            bind(node.arg)
        elif (isinstance(node, DEFS + (ast.ClassDef, ast.ExceptHandler))
              or type(node).__name__ in ('MatchAs', 'MatchStar')):
            bind(node.name)
        elif type(node).__name__ == 'MatchMapping':
            bind(node.rest)
    return bound


def really_exits(func, bound) -> bool:
    """What a call calls ends the process: the builtin `exit` or `quit`, when
    the module binds no such name, or `exit` of sys / `_exit` of os, on a name
    that nothing but a plain import of that module binds. A function or a
    method that is merely called exit ends nothing."""
    if isinstance(func, ast.Name):
        return func.id in BUILTIN_EXITS and func.id not in bound
    return (isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name)
            and any(func.attr == attr and bound.get(func.value.id) == {module}
                    for module, attr in EXIT_FUNCTIONS.items()))


def exits(stmt) -> bool:
    """The statement is a call that really exits, as parsed() marked it."""
    return (isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call)
            and getattr(stmt.value, 'exits', False))


def guard_form(stmt) -> str:
    """The guard this one statement is, or ''."""
    if isinstance(stmt, ast.Return) and is_skip_code(stmt.value):
        return 'return 77'
    if isinstance(stmt, ast.Raise) and stmt.exc is not None:
        raised = stmt.exc.func if isinstance(stmt.exc, ast.Call) else stmt.exc
        name = raised.attr if isinstance(raised, ast.Attribute) else getattr(raised, 'id', '')
        if name == 'SkipTest':
            return 'raise SkipTest'
        if (name == 'SystemExit' and isinstance(stmt.exc, ast.Call)
                and len(stmt.exc.args) == 1 and is_skip_code(stmt.exc.args[0])):
            return 'raise SystemExit(77)'
    if exits(stmt) and len(stmt.value.args) == 1 and is_skip_code(stmt.value.args[0]):
        return callee(stmt.value) + '(77)'
    return ''


def leaves(stmt) -> bool:
    """The statement never hands over to the next statement of its block."""
    if isinstance(stmt, (ast.Return, ast.Raise, ast.Continue, ast.Break)):
        return True
    if isinstance(stmt, ast.Expr):
        return exits(stmt)
    if isinstance(stmt, ast.If):
        if isinstance(stmt.test, ast.Constant):
            return any(map(leaves, stmt.body if stmt.test.value else stmt.orelse))
        return any(map(leaves, stmt.body)) and any(map(leaves, stmt.orelse))
    return False


def sub_blocks(stmt) -> list:
    """The blocks a compound statement runs in its own scope: a def's body is
    another scope, and a branch a constant test rules out never runs."""
    if isinstance(stmt, DEFS):
        return []
    if isinstance(stmt, (ast.If, ast.While)) and isinstance(stmt.test, ast.Constant):
        blocks = [stmt.body] if stmt.test.value else []
        if isinstance(stmt, ast.While) or not stmt.test.value:
            blocks.append(stmt.orelse)
    else:
        blocks = [getattr(stmt, name, None) for name in ('body', 'orelse', 'finalbody')]
        blocks += [handler.body for handler in getattr(stmt, 'handlers', ())]
        blocks += [case.body for case in getattr(stmt, 'cases', ())]
    return [block for block in blocks if block]


def tests_main(stmt) -> bool:
    """An `if` on `__name__`: its branches run only when the module is the
    program, or only when it is not."""
    return isinstance(stmt, ast.If) and '__name__' in names_in(stmt.test)


def first_guard(block):
    """The first guard a block can reach, as (form, line), or None. A guard
    under an `if` on `__name__` is not looked for: the caller asks about a
    line outside that `if`, and a main block does not run for a line a test
    runner reaches by importing the module."""
    for stmt in block:
        form = guard_form(stmt)
        if form:
            return form, stmt.lineno
        for sub in () if tests_main(stmt) else sub_blocks(stmt):
            found = first_guard(sub)
            if found:
                return found
        if leaves(stmt):
            break
    return None


def own_nodes(stmt):
    """The nodes of one statement, without the statements of its blocks."""
    pending = [stmt]
    while pending:
        node = pending.pop()
        yield node
        pending.extend(child for child in ast.iter_child_nodes(node)
                       if not isinstance(child, ast.stmt))


def live_statements(block):
    """Every statement a block can reach, at any depth, the bodies of its
    defs and classes included: nothing in a branch a constant test rules out,
    nothing behind a statement that always leaves its block."""
    for stmt in block:
        yield stmt
        for sub in [stmt.body] if isinstance(stmt, DEFS) else sub_blocks(stmt):
            yield from live_statements(sub)
        if leaves(stmt):
            break


def statement_at(block, line: int):
    """The innermost statement of a scope's own flow that holds `line`, or
    None for a line between statements."""
    for stmt in block:
        if stmt.lineno <= line <= stmt.end_lineno:
            for sub in sub_blocks(stmt):
                if sub[0].lineno <= line <= sub[-1].end_lineno:
                    return statement_at(sub, line)
            return stmt
    return None


def guard_before(block, line: int):
    """Follow a scope's statements, in order, to the one that holds `line`.

    Returns (guard, statement): the first guard anchored before the line, as
    (form, line) or None, and the innermost statement of this scope that
    holds the line (None for a line between statements, and when a guard was
    found first).
    """
    live = True
    for stmt in block:
        if stmt.end_lineno < line:
            if live:
                found = first_guard([stmt])
                if found:
                    return found, None
                live = not leaves(stmt)
            continue
        if stmt.lineno > line:
            break
        if (live and isinstance(stmt, ast.If)
                and stmt.test.lineno <= line <= stmt.test.end_lineno):
            for sub in sub_blocks(stmt):
                found = first_guard(sub)
                if found:
                    return found, stmt
        for sub in sub_blocks(stmt):
            if sub[0].lineno <= line <= sub[-1].end_lineno:
                return guard_before(sub, line)
        return None, stmt
    return None, None


def held_plainly(value, wanted) -> bool:
    """The expression holds a node `wanted` picks, and holds every one of
    them under nothing but operators, containers, f-strings and the arguments
    of a path builder."""
    found = False
    pending = [(value, True)]
    while pending:
        node, plain = pending.pop()
        if wanted(node):
            if not plain:
                return False
            found = True
        elif isinstance(node, ast.Call) and callee(node) in PATH_BUILDERS:
            pending.append((node.func, False))
            pending.extend((arg, plain) for arg in node.args)
            pending.extend((keyword.value, False) for keyword in node.keywords)
        else:
            keeps = isinstance(node, (ast.BinOp, ast.Tuple, ast.List, ast.Set, ast.Dict,
                                      ast.JoinedStr, ast.FormattedValue))
            pending.extend((child, plain and keeps) for child in ast.iter_child_nodes(node))
    return found


def binds_a_path(stmt, line: int) -> bool:
    """The statement only binds a path to a name: every string constant on
    `line` sits in the assigned value under nothing but operators, containers,
    f-strings and the arguments of a path builder."""
    if not isinstance(stmt, (ast.Assign, ast.AnnAssign)) or stmt.value is None:
        return False
    return held_plainly(stmt.value, lambda node: (
        isinstance(node, ast.Constant) and isinstance(node.value, str)
        and node.lineno <= line <= node.end_lineno))


def top_level_calls(block) -> set:
    """The plain names a block's own flow calls: no def or class body, no
    branch that cannot run, nothing behind a statement that always leaves."""
    names = set()
    for stmt in block:
        if not isinstance(stmt, DEFS + (ast.ClassDef,)):
            for sub in sub_blocks(stmt):
                names |= top_level_calls(sub)
            compound = isinstance(getattr(stmt, 'body', None), list) or hasattr(stmt, 'cases')
            parts = ([child for child in ast.iter_child_nodes(stmt)
                      if isinstance(child, ast.expr)] if compound else [stmt])
            names |= {node.func.id for part in parts for node in ast.walk(part)
                      if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)}
        if leaves(stmt):
            break
    return names


@functools.lru_cache(maxsize=None)
def parsed(text: str):
    """The module's tree, its defs and classes, the names its top level binds
    to a skipUnless(...) call, and whether a string constant holds the pinned
    literal. Every call of the tree is marked with whether it really exits
    (really_exits), which is what guard_form and leaves read."""
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        tree = ast.parse(text)
    nodes = list(ast.walk(tree))
    bound = bound_names(tree)
    for node in nodes:
        if isinstance(node, ast.Call):
            node.exits = really_exits(node.func, bound)
    around = [node for node in nodes if isinstance(node, DEFS + (ast.ClassDef,))]
    aliases = frozenset(
        target.id for stmt in tree.body
        if isinstance(stmt, ast.Assign) and isinstance(stmt.value, ast.Call)
        and callee(stmt.value) == 'skipUnless'
        for target in stmt.targets if isinstance(target, ast.Name))
    literal = any(isinstance(node, ast.Constant) and isinstance(node.value, str)
                  and SKIP_LITERAL in node.value for node in nodes)
    return tree, around, aliases, literal


@functools.lru_cache(maxsize=None)
def bindings(text: str) -> dict:
    """bound_names of the module `text`."""
    return bound_names(parsed(text)[0])


@functools.lru_cache(maxsize=None)
def parents_of(text: str) -> dict:
    """Each node of the module's tree, mapped to the node that holds it."""
    tree = parsed(text)[0]
    return {child: node for node in ast.walk(tree) for child in ast.iter_child_nodes(node)}


def owner(node, parents):
    """The innermost def around a node, its decorators, defaults and
    annotations included; None at module level."""
    node = parents.get(node)
    while node is not None and not isinstance(node, DEFS):
        node = parents.get(node)
    return node


def def_part(node, function, parents):
    """Where in a def a node sits: the statement of its body, or the default
    value that holds it. None for a decorator or an annotation, which run
    where the def stands."""
    path = [node]
    while parents.get(path[-1]) is not function:
        path.append(parents[path[-1]])
    top = path[-1]
    if top is function.args and len(path) > 1:
        defaults = function.args.defaults + function.args.kw_defaults
        return path[-2] if any(path[-2] is default for default in defaults) else None
    return top if any(top is stmt for stmt in function.body) else None


def names_in(node) -> set:
    """Every name an expression or a statement mentions."""
    return {child.id for child in ast.walk(node) if isinstance(child, ast.Name)}


def calls_in(nodes) -> set:
    """The plain names the nodes call."""
    return {node.func.id for part in nodes for node in ast.walk(part)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)}


def flow_statements(block):
    """Every statement of a scope's own flow, at any depth. The body of a def
    or of a class binds names of another scope and is left out."""
    for stmt in block:
        yield stmt
        if isinstance(stmt, DEFS + (ast.ClassDef,)):
            continue
        for name in ('body', 'orelse', 'finalbody'):
            yield from flow_statements(getattr(stmt, name, None) or ())
        for part in (*getattr(stmt, 'handlers', ()), *getattr(stmt, 'cases', ())):
            yield from flow_statements(part.body)


def assigned(stmt):
    """The targets and the value of an assignment statement, or ((), None)."""
    if isinstance(stmt, ast.Assign):
        return stmt.targets, stmt.value
    if isinstance(stmt, (ast.AnnAssign, ast.AugAssign)) and stmt.value is not None:
        return [stmt.target], stmt.value
    return (), None


def followed(name: str, block):
    """The names that stand for one bound path in its scope: the bound name,
    and every name an assignment of the same scope only passes it along to
    (held_plainly). Returns them with the uses that do the passing along,
    which open nothing."""
    names = {name}

    def passing(stmt) -> bool:
        targets, value = assigned(stmt)
        return (isinstance(stmt, (ast.Assign, ast.AnnAssign)) and value is not None
                and all(isinstance(target, ast.Name) for target in targets)
                and held_plainly(value, lambda node: isinstance(node, ast.Name)
                                 and node.id in names))

    while True:
        more = {target.id for stmt in flow_statements(block) if passing(stmt)
                for target in assigned(stmt)[0]} - names
        if not more:
            break
        names |= more
    passed = {node for stmt in flow_statements(block) if passing(stmt)
              for node in ast.walk(assigned(stmt)[1])
              if isinstance(node, ast.Name) and node.id in names}
    return names, passed


def probes(block):
    """The guarded tests of a scope's own flow: every `if` that can run and
    holds a guard as a statement of its own body or else-branch, with nothing
    before the guard in that branch that always leaves. Yields the `if`, the
    block it stands in, the guard's form, the guard's line, and whether the
    guard is in the body (True) or in the else-branch (False)."""
    for stmt in block:
        if isinstance(stmt, ast.If):
            for branch in sub_blocks(stmt):
                for inner in branch:
                    form = guard_form(inner)
                    if form:
                        yield stmt, block, form, inner.lineno, branch is stmt.body
                    if form or leaves(inner):
                        break
        for sub in sub_blocks(stmt):
            yield from probes(sub)
        if leaves(stmt):
            break


def fed_by(test, block, names):
    """An `if` test and what feeds it, read backwards from the test through
    the statements of the scope that stand before it: an assignment to a name
    the test needs, the sequence of a `for` whose target it needs, and a call
    statement on a name it needs (`parser.add_argument(...)`) -- unless that
    name is one of the followed `names`, because a call on the bound path is
    a use of it. Returns the expressions (the test, the assigned values, the
    sequences, the calls), every name they mention, and the `for` statements
    whose sequence was taken. What it returns names the path; it excuses no
    read (bound_gate asks opens_nothing of each expression)."""
    needed = names_in(test)
    parts = [test]
    loops = []
    before = sorted((stmt for stmt in flow_statements(block) if stmt.lineno < test.lineno),
                    key=lambda stmt: stmt.lineno, reverse=True)
    for stmt in before:
        targets, value = assigned(stmt)
        if isinstance(stmt, (ast.For, ast.AsyncFor)):
            if needed & names_in(stmt.target):
                parts.append(stmt.iter)
                loops.append(stmt)
                needed |= names_in(stmt.iter)
        elif stmt.end_lineno >= test.lineno:
            continue
        elif value is not None:
            if needed & set().union(*map(names_in, targets)):
                parts.append(value)
                needed |= names_in(value)
        elif (isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call)
              and isinstance(stmt.value.func, ast.Attribute)
              and isinstance(stmt.value.func.value, ast.Name)
              and stmt.value.func.value.id in needed - names):
            parts.append(stmt.value)
            needed |= names_in(stmt.value)
    return parts, needed, loops


def existence_subject(node, bound):
    """The path an existence test asks about, or None: P in `P.exists()`,
    `P.is_file()`, `P.is_dir()`, and in `os.path.exists(P)` (isfile, isdir,
    lexists) where the first name is bound by a plain import of os."""
    if (not isinstance(node, ast.Call) or node.keywords
            or not isinstance(node.func, ast.Attribute)):
        return None
    func = node.func
    if not node.args:
        return func.value if func.attr in EXISTENCE_METHODS else None
    holder = func.value
    if (len(node.args) == 1 and func.attr in EXISTENCE_FUNCTIONS
            and isinstance(holder, ast.Attribute) and holder.attr == 'path'
            and isinstance(holder.value, ast.Name) and bound.get(holder.value.id) == {'os'}):
        return node.args[0]
    return None


def stands_for(node, carriers) -> bool:
    """The expression is the bound path or a path under it: one of the names
    that carry it, alone, joined with `/`, or handed to a path builder."""
    if isinstance(node, ast.Name):
        return node.id in carriers
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
        return stands_for(node.left, carriers) or stands_for(node.right, carriers)
    if isinstance(node, ast.Call) and callee(node) in PATH_BUILDERS and not node.keywords:
        return any(stands_for(arg, carriers) for arg in node.args)
    return False


def tied(node, block, carriers, names) -> bool:
    """The expression an existence test asks about is the bound path
    (stands_for), or it mentions no name that carries the path and what the
    scope assigns to the names it does mention leads back to one (fed_by):
    `provider['manifest']` after `provider = next(... PROVIDERS.values() ...)`.
    An attribute of the path (`P.parent`) is another path and is neither."""
    if stands_for(node, carriers):
        return True
    return not names_in(node) & carriers and bool(names & fed_by(node, block, names)[1])


def opens_nothing(node, carriers, bound) -> bool:
    """No name that carries the bound path is handed to anything that could
    open it. A carrier may stand alone, under operators, comparisons,
    containers, f-strings, subscripts and conditional expressions; as what an
    existence test asks about; as an argument of a path builder or of a
    builtin that only reads its arguments (PURE_BUILTINS, where the module
    binds no such name); as what a dictionary view is taken of
    (`PROVIDERS.values()`); and as the `default=` of an `add_argument(...)`
    call, which keeps the value. The variable of a comprehension that loops
    over a carrier carries the path inside the comprehension. Any other call,
    and any attribute, is taken to open it."""
    if isinstance(node, ast.Name) or not names_in(node) & carriers:
        return True
    subject = existence_subject(node, bound)
    if subject is not None:
        return opens_nothing(subject, carriers, bound)
    if isinstance(node, (ast.ListComp, ast.SetComp, ast.GeneratorExp, ast.DictComp)):
        inside = set(carriers)
        for loop in node.generators:
            if not opens_nothing(loop.iter, inside, bound):
                return False
            if names_in(loop.iter) & inside:
                inside |= names_in(loop.target)
            if not all(opens_nothing(test, inside, bound) for test in loop.ifs):
                return False
        results = [node.key, node.value] if isinstance(node, ast.DictComp) else [node.elt]
        return all(opens_nothing(result, inside, bound) for result in results)
    if isinstance(node, ast.Call):
        func = node.func
        handed = [*node.args, *(keyword.value for keyword in node.keywords)]
        if isinstance(func, ast.Attribute) and func.attr in CONTAINER_VIEWS:
            kept, handed = handed, [func.value]
        elif isinstance(func, ast.Attribute) and func.attr == 'add_argument':
            kept = [func, *node.args, *(keyword.value for keyword in node.keywords
                                        if keyword.arg != 'default')]
            handed = [keyword.value for keyword in node.keywords if keyword.arg == 'default']
        elif (callee(node) in PATH_BUILDERS or isinstance(func, ast.Name)
              and func.id in PURE_BUILTINS and func.id not in bound):
            kept = [func]
        else:
            return False
        return (not any(names_in(part) & carriers for part in kept)
                and all(opens_nothing(part, carriers, bound) for part in handed))
    if isinstance(node, PLAIN_HOLDERS):
        return all(opens_nothing(child, carriers, bound)
                   for child in ast.iter_child_nodes(node) if isinstance(child, ast.expr))
    return False


def values_bound(name: str, block):
    """The values a scope's own flow assigns to one plain name, or None when
    the name is bound there in any other way (a loop target, an augmented
    assignment, a tuple target, a walrus)."""
    values = []
    for stmt in flow_statements(block):
        if not any(isinstance(node, ast.Name) and node.id == name
                   and not isinstance(node.ctx, ast.Load) for node in own_nodes(stmt)):
            continue
        targets, value = assigned(stmt)
        if (not isinstance(stmt, (ast.Assign, ast.AnnAssign)) or value is None
                or not all(isinstance(target, ast.Name) for target in targets)):
            return None
        values.append(value)
    return values


def absent_truth(node, block, carriers, context, seen=frozenset()):
    """The truth of an expression when the bound path is absent: True, False,
    or None when the expression does not settle it. `block` is the scope the
    expression stands in, `carriers` the names that carry the path there, and
    `context` the followed names, the module-level functions that may be
    looked into, and the names the module binds.

    Read: `not`, `and` / `or`; an existence test of the path (False); a list,
    tuple or set display (empty or not); a conditional expression whose test
    is settled; a list or set comprehension with one loop over the path whose
    filters all hold for an absent path (True: the list of the absent ones);
    any(...) / all(...) over such a loop; a plain name, through every value
    the scope assigns to it; and a call of a module-level function, through
    every value it returns (one level)."""
    names, helpers, bound = context

    def again(part, inside=carriers, known=seen):
        return absent_truth(part, block, inside, context, known)

    def same(truths):
        return truths.pop() if len(truths) == 1 else None

    def looped(comprehension):
        """The carriers inside a comprehension whose one loop passes over
        the bound path, or None."""
        if len(comprehension.generators) != 1:
            return None
        loop = comprehension.generators[0]
        items = (loop.iter.elts if isinstance(loop.iter, (ast.Tuple, ast.List, ast.Set))
                 else [loop.iter])
        if (loop.is_async or not isinstance(loop.target, ast.Name)
                or not any(tied(item, block, carriers, names) for item in items)):
            return None
        return carriers | {loop.target.id}

    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
        truth = again(node.operand)
        return None if truth is None else not truth
    if isinstance(node, ast.BoolOp):
        truths = [again(value) for value in node.values]
        settles = isinstance(node.op, ast.Or)
        if settles in truths:
            return settles
        return None if None in truths else not settles
    subject = existence_subject(node, bound)
    if subject is not None:
        return False if tied(subject, block, carriers, names) else None
    if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
        return None if any(isinstance(item, ast.Starred) for item in node.elts) else bool(node.elts)
    if isinstance(node, ast.IfExp):
        truth = again(node.test)
        return None if truth is None else again(node.body if truth else node.orelse)
    if isinstance(node, (ast.ListComp, ast.SetComp)):
        inside = looped(node)
        if inside is not None and all(again(test, inside) is True
                                      for test in node.generators[0].ifs):
            return True
        return None
    if isinstance(node, ast.Name):
        values = None if node.id in seen else values_bound(node.id, block)
        if not values:
            return None
        return same({again(value, known=seen | {node.id}) for value in values})
    if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
        return None
    called = node.func.id
    if (called in ('any', 'all') and called not in bound and len(node.args) == 1
            and not node.keywords
            and isinstance(node.args[0], (ast.GeneratorExp, ast.ListComp))):
        inside = looped(node.args[0])
        settles = called == 'any'
        if (inside is not None and not node.args[0].generators[0].ifs
                and again(node.args[0].elt, inside) is settles):
            return settles
        return None
    if called in helpers and called not in seen:
        helper = helpers[called]
        returned = [stmt.value for stmt in flow_statements(helper.body)
                    if isinstance(stmt, ast.Return)]
        if not returned or None in returned or not leaves(helper.body[-1]):
            return None
        return same({absent_truth(value, helper.body, carriers, (names, {}, bound),
                                  seen | {called}) for value in returned})
    return None


def bound_gate(text: str, scope, statement, gate) -> Gate:
    """The verdict for a path that `statement` only binds to a name, in the
    function or module `scope`: rule 2 of the comment above. `gate` turns the
    guard that holds into the verdict."""
    tree = parsed(text)[0]
    parents = parents_of(text)
    bound = bindings(text)
    targets = assigned(statement)[0]
    if len(targets) != 1 or not isinstance(targets[0], ast.Name):
        return Gate(False, 'the path is not bound to one plain name, so its uses '
                           'cannot be followed')
    if isinstance(parents.get(statement), ast.ClassDef):
        return Gate(False, 'the path is bound in a class body, where its uses cannot '
                           'be followed')
    name = targets[0].id
    names, passed = followed(name, scope.body)
    uses = sorted((node for node in ast.walk(scope)
                   if isinstance(node, ast.Name) and node.id in names
                   and isinstance(node.ctx, ast.Load) and node not in passed),
                  key=lambda node: (node.lineno, node.col_offset))
    # A module-level function sees the names of the module, not the locals
    # of another function: it is looked into for a module-level binding only.
    helpers = ({stmt.name: stmt for stmt in tree.body if isinstance(stmt, DEFS)}
               if scope is tree else {})
    places = [(scope, 'in the scope that binds the path to a name')]
    if scope is tree:
        called = top_level_calls(tree.body)
        places += [(function, f'in {function.name}(), which the module top level runs')
                   for function in tree.body
                   if isinstance(function, DEFS) and function.name in called]
    refused = ''
    for place, where in places:
        for test_if, block, form, at, in_body in probes(place.body):
            parts, needed, loops = fed_by(test_if.test, place.body, names)
            asked = [helpers[helper] for helper in sorted(calls_in(parts)) if helper in helpers]
            if not (names & needed or any(names & names_in(helper) for helper in asked)):
                refused = refused or (f'{form} at line {at} {where}, but its test does '
                                      f'not name {name}')
                continue
            # The variable of a `for` that feeds the test carries the path
            # through that loop: what the loop does with it is a use.
            aliases = {alias for loop in loops if names_in(loop.iter) & names
                       for alias in names_in(loop.target)}
            carriers = names | aliases
            if absent_truth(test_if.test, place.body, carriers,
                            (names, helpers, bound)) is not in_body:
                refused = refused or (f'{form} at line {at} {where}, but its test is not '
                                      'an existence test that sends an absent '
                                      f'{name} to the guard')
                continue
            for helper in asked:
                parts += [stmt.value for stmt in flow_statements(helper.body)
                          if isinstance(stmt, ast.Return) and stmt.value is not None]
            # The test and what feeds it excuse a carrier only where they
            # open nothing: feeding the test is no excuse for a read.
            probe = {node for part in parts if opens_nothing(part, carriers, bound)
                     for node in ast.walk(part)
                     if isinstance(node, ast.Name) and node.id in carriers}
            behind = {node for later in (*test_if.body, *test_if.orelse,
                                         *block[block.index(test_if) + 1:])
                      for node in ast.walk(later)}
            watched = uses + [node for loop in loops
                              for stmt in (*loop.body, *loop.orelse) for node in ast.walk(stmt)
                              if isinstance(node, ast.Name) and node.id in aliases
                              and isinstance(node.ctx, ast.Load)]

            def dominated(node, entered=()):
                """True or False; None for a reference from a function that
                is already being followed, which settles nothing."""
                if node in probe or node in behind:
                    return True
                function = owner(node, parents)
                if function is None or function is place:
                    return False
                if function in entered:
                    return None
                part = def_part(node, function, parents)
                if (part is None or isinstance(parents.get(function), ast.ClassDef)
                        or not isinstance(part, ast.stmt) and not held_plainly(
                            part, lambda held: isinstance(held, ast.Name)
                            and held.id in names)):
                    return False
                home = owner(function, parents) or tree
                verdicts = [dominated(reference, entered + (function,))
                            for reference in ast.walk(home)
                            if isinstance(reference, ast.Name)
                            and reference.id == function.name
                            and isinstance(reference.ctx, ast.Load)]
                # Something has to refer to the function from behind the
                # guard: one nothing refers to may be called by a test runner.
                return False not in verdicts and True in verdicts

            loose = next((use for use in watched if not dominated(use)), None)
            if loose is None:
                return gate((form, at), where)
            refused = refused or (f'{form} at line {at} {where}, but it does not stand '
                                  f'before the use of {loose.id} at line {loose.lineno}')
    return Gate(False, refused or f'the path is bound to {name}, and neither its scope nor '
                                  'a function the module top level runs holds a reachable '
                                  'guard under an `if`')


def read_gate(text: str, line: int) -> Gate:
    """Whether the module `text` gates the private path it names at `line`,
    and the guard that does or the reason none does. A module that does not
    parse gates nothing."""
    try:
        tree, around, aliases, literal = parsed(text)
    except (SyntaxError, ValueError, RecursionError) as error:
        return Gate(False, 'the module does not parse, so no guard can be read '
                           f'from it: {error}')

    def gate(guard, where: str) -> Gate:
        form, at = guard
        if form != 'raise SkipTest' and not literal:
            return Gate(False, f'{form} at line {at} {where}, but no string constant '
                               f'of the module holds {SKIP_LITERAL!r}')
        return Gate(True, f'{form} at line {at} {where}')

    holders = sorted((node for node in around if node.lineno <= line <= node.end_lineno),
                     key=lambda node: node.lineno)
    functions = [node for node in holders if isinstance(node, DEFS)]
    if functions:
        for node in holders[:holders.index(functions[-1]) + 1]:
            for decorator in node.decorator_list:
                if (isinstance(decorator, ast.Call) and callee(decorator) == 'skipUnless'
                        or isinstance(decorator, ast.Name) and decorator.id in aliases):
                    return Gate(True, f'skipUnless on {node.name} (line {node.lineno})')
    # A line that only binds the path opens nothing, and a guard above it
    # cannot test the name it binds: the bound rule alone answers for it.
    scope = functions[-1] if functions else tree
    statement = statement_at(scope.body, line)
    if statement is not None and binds_a_path(statement, line):
        return bound_gate(text, scope, statement, gate)
    for around_line in [tree] + functions:
        guard, _ = guard_before(around_line.body, line)
        if guard:
            return gate(guard, 'before the read')
    return Gate(False, 'no reachable guard stands before the line that names the path')


def module_gates_its_own_paths(text: str, line: int) -> bool:
    """The module that names a private path at `line` reaches a guard before
    it reads the path -- the settlement-ABI family's own gate (95c27eaff).
    The guard is read from the module's syntax tree (read_gate): text in a
    string constant or a comment is not a guard."""
    return read_gate(text, line).guarded


@functools.lru_cache(maxsize=None)
def skip_parts(text: str):
    """What the row's skip path reads from one module of the closure, as
    (literal, skip): whether a string constant holds the pinned literal, and
    whether the module holds a unittest skip that can run -- a `raise
    SkipTest` statement (also as unittest.SkipTest(...)) or a call of
    skipUnless, in a statement the module can reach (live_statements: not
    under a test that never holds, not behind a statement that always
    leaves; the bodies of defs and classes count). The same words in a string
    constant or a comment are neither, and a module that does not parse holds
    nothing."""
    try:
        tree, _, _, literal = parsed(text)
    except (SyntaxError, ValueError, RecursionError):
        return False, False
    skip = any(guard_form(stmt) == 'raise SkipTest'
               or any(isinstance(node, ast.Call) and callee(node) == 'skipUnless'
                      for node in own_nodes(stmt))
               for stmt in live_statements(tree.body))
    return literal, skip


def has_skip_path(properties, closure, unguarded=()) -> bool:
    """The row's own skip path. A string constant of some module of the
    closure holds the pinned literal, and one of: the row's SKIP_RETURN_CODE
    is 77; its SKIP_REGULAR_EXPRESSION names the literal; or every module
    that names an unguarded private path (`unguarded`, as admitted_by lists
    them) holds a unittest skip that can run (skip_parts). A skip in another
    module of the closure answers for nothing, and whether the skip stands
    around the read is not checked. Literal and skip are read from the
    syntax trees, not from the text."""
    parts = {path: skip_parts(text) for path, text in closure.items()}
    if not any(literal for literal, _ in parts.values()):
        return False
    if properties.get('SKIP_RETURN_CODE') == '77':
        return True
    if 'private fixtures not present' in properties.get('SKIP_REGULAR_EXPRESSION', ''):
        return True
    readers = {path for path, _, _ in unguarded}
    return bool(readers) and all(parts[path][1] for path in readers)


def admitted_by(name, properties, closure, source=None):
    """The rule that admits one program of a row, and the reads its own
    modules do not gate. The rules, in the order they are asked: 'no private
    read', 'self-gating' (every read is gated by the module that names it),
    'exempt' (the row's EXEMPT entry), 'skip path' (has_skip_path); None when
    no rule admits the program. An inline program (`source`) has no module of
    its own, so its reads are never self-gating."""
    reads = private_reads(closure)
    unguarded = [read for read in reads
                 if read[0] == source
                 or not module_gates_its_own_paths(closure[read[0]], read[1])]
    if not reads:
        return 'no private read', unguarded
    if not unguarded:
        return 'self-gating', unguarded
    if name in EXEMPT:
        return 'exempt', unguarded
    if has_skip_path(properties, closure, unguarded):
        return 'skip path', unguarded
    return None, unguarded


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
                rule, unguarded = admitted_by(name, properties.get(name, {}), closure, source)
                if rule is None:
                    path, line, reference = unguarded[0]
                    if path == source:
                        reason = 'an inline program has no module of its own'
                        line += program.line - 1
                    else:
                        reason = read_gate(closure[path], line).reason
                    offenders.append((name, (path, line, reference), reason))
        except UnknownCommand as error:
            name, message = error.args
            print(f'NOT COMPARED: {message}', file=sys.stderr)
            unknown_command |= '${' not in name
        except (OSError, SweepError) as error:
            errors.append(str(error))
    for name, (path, line, reference), reason in offenders:
        print(f'{name}: reads absent {reference} ({path}:{line}) '
              f'without printing {SKIP_LITERAL!r} and skipping')
        print(f'{name}: {path}:{line} is not gated by its own module: {reason}')
    for name, reason in sorted(EXEMPT.items()):
        print(f'exempt: {name} -- {reason}')
    if not programs_seen and not errors:
        errors.append('no registered Python programs')
    if errors or unknown_command:
        for error in errors:
            print(f'check_kernel_python_private_reads: NOT COMPARED: {error}', file=sys.stderr)
        return 2
    count = len({name for name, *_ in offenders})
    print(f'kernel python rows reading an absent private file: {count}')
    return 1 if offenders else 0


if __name__ == '__main__':
    raise SystemExit(main())
