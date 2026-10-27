#!/usr/bin/env python3
"""Check the per-lane CMake registration directory."""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CMAKE = ROOT / "tests" / "CMakeLists.txt"
LANE_DIR = ROOT / "tests" / "cr150.d"
README = LANE_DIR / "README.md"
FAST_TEST_REGISTRATION = '''foreach(_pf_fast_test IN LISTS PINEFORGE_PR_FAST_TESTS)
    if(PINEFORGE_BUILD_SOURCE_LAYER AND (NOT TEST ${_pf_fast_test} OR NOT TARGET ${_pf_fast_test}))
        message(FATAL_ERROR "PR fast row or executable is missing: ${_pf_fast_test}")
    endif()
    if(TEST ${_pf_fast_test})
        get_test_property(${_pf_fast_test} DISABLED _pf_fast_disabled)
        get_test_property(${_pf_fast_test} SKIP_RETURN_CODE _pf_fast_skip_code)
        get_test_property(${_pf_fast_test} SKIP_REGULAR_EXPRESSION _pf_fast_skip_regex)
        get_test_property(${_pf_fast_test} LABELS _pf_fast_labels)
        if(_pf_fast_disabled OR NOT _pf_fast_skip_code STREQUAL "NOTFOUND"
                OR NOT _pf_fast_skip_regex STREQUAL "NOTFOUND"
                OR "slow" IN_LIST _pf_fast_labels OR "private_fixtures" IN_LIST _pf_fast_labels)
            message(FATAL_ERROR "PR fast row must execute without skips: ${_pf_fast_test}")
        endif()
        set_property(TEST ${_pf_fast_test} APPEND PROPERTY LABELS fast)
    endif()
endforeach()
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_custom_target(pineforge_pr_smoke DEPENDS ${PINEFORGE_PR_FAST_TESTS})
endif()
'''
ALLOWED_TEST_PROPERTIES = frozenset({
    # TIMEOUT is parsed here for ownership diagnostics, then rejected by the
    # cross-file owner-#827 rule after all lane syntax has been checked.
    "LABELS", "SKIP_RETURN_CODE", "TIMEOUT", "RUN_SERIAL", "DEPENDS",
})
REFUSED_TEST_PROPERTIES = frozenset({
    "ENVIRONMENT", "WORKING_DIRECTORY", "ENVIRONMENT_MODIFICATION",
})
ARGUMENT_LITERAL = (
    r'"(?:\\.|[^"\\])*"|'
    r'\[(?P<bracket_level>=*)\[(?P<bracket_value>.*?)\](?P=bracket_level)\]'
)
ARGUMENT_RE = re.compile(ARGUMENT_LITERAL, re.DOTALL)
COMMENT_OR_ARGUMENT_RE = re.compile(ARGUMENT_LITERAL + r'|#[^\n]*', re.DOTALL)
FENCE_COMMENT_OR_ARGUMENT_RE = re.compile(
    r'#\[(?P<comment_level>=*)\[.*?\](?P=comment_level)\]|' +
    ARGUMENT_LITERAL + r'|#[^\n]*', re.DOTALL)
LITERAL_NAME_RE = re.compile(r"[A-Za-z0-9_.+-]+")
TARGET_COMMANDS = frozenset({
    "target_compile_definitions", "target_compile_options",
    "target_link_libraries", "target_link_options",
    "target_include_directories", "target_sources",
})
# This exact terminal validation is the sole unfenced tail exception.
# Keep standalone checker controls usable without importing the CI driver.
# The CI guard-gap test compares it with ci_preflight's pinned block.
FINAL_FAST_VALIDATION = '''foreach(_pf_fast_test IN LISTS PINEFORGE_PR_FAST_TESTS)
    if(PINEFORGE_BUILD_SOURCE_LAYER AND (NOT TEST ${_pf_fast_test} OR NOT TARGET ${_pf_fast_test}))
        message(FATAL_ERROR "PR fast row or executable is missing: ${_pf_fast_test}")
    endif()
    if(TEST ${_pf_fast_test})
        get_test_property(${_pf_fast_test} DISABLED _pf_fast_disabled)
        get_test_property(${_pf_fast_test} SKIP_RETURN_CODE _pf_fast_skip_code)
        get_test_property(${_pf_fast_test} SKIP_REGULAR_EXPRESSION _pf_fast_skip_regex)
        get_test_property(${_pf_fast_test} LABELS _pf_fast_labels)
        if(_pf_fast_disabled OR NOT _pf_fast_skip_code STREQUAL "NOTFOUND"
                OR NOT _pf_fast_skip_regex STREQUAL "NOTFOUND"
                OR "slow" IN_LIST _pf_fast_labels OR "private_fixtures" IN_LIST _pf_fast_labels)
            message(FATAL_ERROR "PR fast row must execute without skips: ${_pf_fast_test}")
        endif()
        set_property(TEST ${_pf_fast_test} APPEND PROPERTY LABELS fast)
    endif()
endforeach()
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_custom_target(pineforge_pr_smoke DEPENDS ${PINEFORGE_PR_FAST_TESTS})
endif()
'''


class CheckFailure(Exception):
    pass


@dataclass(frozen=True)
class Statement:
    name: str
    body: str
    line: int


@dataclass(frozen=True)
class FenceMarker:
    lane: str
    edge: str
    start: int
    end: int
    line: int


@dataclass(frozen=True)
class Fence:
    lane: str
    statements: list[Statement]
    line: int


def _without_comments(text: str, *, bracket_comments: bool = False) -> str:
    """Remove comments, preserving quoted/bracketed arguments and lines."""
    pattern = FENCE_COMMENT_OR_ARGUMENT_RE if bracket_comments else COMMENT_OR_ARGUMENT_RE
    return pattern.sub(
        lambda match: re.sub(r"[^\n]", " " if bracket_comments else "", match.group(0))
        if match.group(0).startswith("#") else match.group(0), text)


def _statements(path: Path, text: str, *, mask_quotes: bool = False) -> list[Statement]:
    """Read balanced, line-started CMake calls with their source lines."""
    masked = _without_comments(text, bracket_comments=mask_quotes)
    # Bracket contents are literal argument text, including #, parentheses,
    # and lines that look like commands. Keep their positions for diagnostics.
    syntax = ARGUMENT_RE.sub(
        lambda match: re.sub(r"[^\n]", " ", match.group(0))
        if mask_quotes or match.group(0).startswith("[") else match.group(0), masked)
    starts = list(re.finditer(r"(?m)^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(", syntax))
    statements: list[Statement] = []
    cursor = 0
    for match in starts:
        between = masked[cursor:match.start()]
        if between.strip():
            line = masked.count("\n", 0, match.start(1)) + 1
            raise CheckFailure(f"{path}:{line}: text is not a CMake statement")
        open_at = match.end() - 1
        depth = 0
        quoted = False
        escaped = False
        close_at: int | None = None
        for index in range(open_at, len(syntax)):
            char = syntax[index]
            if escaped:
                escaped = False
                continue
            if char == "\\" and quoted:
                escaped = True
                continue
            if char == '"':
                quoted = not quoted
                continue
            if quoted:
                continue
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    close_at = index
                    break
        if close_at is None:
            line = masked.count("\n", 0, open_at) + 1
            raise CheckFailure(f"{path}:{line}: unterminated {match.group(1)}(...)")
        body = masked[open_at + 1:close_at]
        line = masked.count("\n", 0, match.start(1)) + 1
        statements.append(Statement(match.group(1), body, line))
        cursor = close_at + 1
    if masked[cursor:].strip():
        line = masked.count("\n", 0, cursor) + 1
        raise CheckFailure(f"{path}:{line}: text is not a CMake statement")
    return statements


def _fence_words(statement: Statement, *, limit: int | None = None) -> list[str]:
    """Parse CMake arguments, including embedded quotes and unquoted lists.

    Write destinations must be checked after argument parsing: a quoted or
    bracketed name is still a name, and an unquoted ';' adds another argument.
    This parser is for fenced statements and ownership discovery only; the
    lane-file command/property policy below remains unchanged.
    """
    body = statement.body
    words: list[str] = []
    index = 0
    while index < len(body):
        if limit is not None and len(words) >= limit:
            return words[:limit]
        if body[index].isspace():
            index += 1
            continue
        if body[index] in "()":
            words.append(body[index])
            index += 1
            continue
        literal = ARGUMENT_RE.match(body, index) if body[index] in '["' else None
        if literal:
            raw = literal.group(0)
            value = raw[1:-1] if raw.startswith('"') else literal.group("bracket_value")
            # CMake ignores a newline immediately after a bracket opener.
            if raw.startswith("[") and value.startswith("\n"):
                value = value[1:]
            words.append(value)
            index = literal.end()
            if (limit is None and index < len(body) and
                    not (body[index].isspace() or body[index] in "()")):
                raise CheckFailure(f"{CMAKE}:{statement.line}: fence arguments need whitespace separation")
            continue
        if body[index] == '"' or re.match(r"\[=*\[", body[index:]):
            raise CheckFailure(f"{CMAKE}:{statement.line}: unterminated fence argument")
        word: list[str] = []
        quoted = False
        present = False
        while index < len(body):
            char = body[index]
            if not quoted and (char.isspace() or char in "()"):
                break
            if char == "\\" and index + 1 < len(body):
                index += 1
                word.append(body[index])
                present = True
            elif char == '"':
                quoted = not quoted
                # Quotes embedded in an unquoted CMake argument are literal
                # characters (the legacy -DKEY="value" form).
                word.append(char)
                present = True
            elif char == ";" and not quoted:
                if present:
                    words.append("".join(word))
                word = []
                present = False
            else:
                word.append(char)
                present = True
            index += 1
        if quoted:
            raise CheckFailure(f"{CMAKE}:{statement.line}: unterminated fence argument")
        if present:
            words.append("".join(word))
    return words if limit is None else words[:limit]


def _words(body: str) -> list[str]:
    return [token[1:-1] if len(token) >= 2 and token[0] == token[-1] == '"' else token
            for token in re.findall(r'"(?:\\.|[^"\\])*"|[^\s]+', body)]


def _property_words(path: Path, statement: Statement) -> list[str]:
    """Property arguments: quoted/bracketed lists stay single arguments."""
    body = statement.body
    words: list[str] = []
    index = 0
    while index < len(body):
        if body[index].isspace():
            index += 1
            continue
        literal = ARGUMENT_RE.match(body, index)
        if literal:
            raw = literal.group(0)
            words.append(raw[1:-1] if raw.startswith('"') else literal.group('bracket_value'))
            index = literal.end()
            if index < len(body) and not body[index].isspace():
                raise CheckFailure(f"{path}:{statement.line}: property arguments need whitespace separation")
            continue
        if body[index] == '"' or re.match(r"\[=*\[", body[index:]):
            raise CheckFailure(f"{path}:{statement.line}: unterminated property argument")
        # CMake divides unquoted lists into arguments. An unquoted ';' must
        # not conceal the next property key; escaped semicolons stay literal.
        word: list[str] = []
        while index < len(body) and not body[index].isspace():
            char = body[index]
            if char == "\\" and index + 1 < len(body):
                index += 1
                word.append(body[index])
            elif char == ";":
                if word:
                    words.append("".join(word))
                    word = []
            elif char == '"':
                raise CheckFailure(f"{path}:{statement.line}: unreadable property argument")
            elif char == "$":
                raise CheckFailure(f"{path}:{statement.line}: unquoted property expansion is not allowed; quote the argument")
            else:
                word.append(char)
            index += 1
        if word:
            words.append("".join(word))
    return words


def _slice_count(source: Path) -> int:
    """Recognize the bounded single-digit slice parser and its dispatch."""
    text = re.sub(
        r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
        lambda match: ' ' if match.group(0).startswith(('//', '/*'))
        else match.group(0), source.read_text(), flags=re.DOTALL)
    counts = re.findall(r'\bconstexpr\s+int\s+kSliceCount\s*=\s*(\d+)\s*;', text)
    code = re.sub(r'\s+', '', text)
    parser = re.search(
        r"intmain\(intargc,char\*\*argv\)\{intslice=0;"
        r"if\(argc!=1\)\{if\((.*?)\)\{([^{}]*)\}"
        r"slice=argv\[2\]\[0\]-'0';\}", code)
    invalid = (
        'argc!=3||std::strcmp(argv[1],"--slice")!=0'
        "||std::strlen(argv[2])!=3"
        "||argv[2][0]<'1'||argv[2][0]>'0'+kSliceCount"
        "||argv[2][1]!='/'||argv[2][2]!='0'+kSliceCount")
    dispatch = re.search(
        r"for\(constauto&(\w+):\w+\)\{"
        r"if\(slice!=0&&\1\.slice!=slice\)continue;", code)
    if (len(counts) != 1 or not 1 <= int(counts[0]) <= 9
            or parser is None or parser[1] != invalid
            or 'return2;' not in parser[2] or dispatch is None):
        raise CheckFailure(f"{source}: does not define supported --slice selection")
    return int(counts[0])


def _require_source(path: Path, line: int, name: str,
                    command: list[str] | None = None) -> None:
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
        raise CheckFailure(f"{path}:{line}: registered name {name!r} is not a literal")
    source = ROOT / "tests" / f"{name}.cpp"
    if source.is_file():
        return
    sliced = re.fullmatch(r"([A-Za-z0-9_.+-]+)_slice_(\d+)", name)
    if sliced is None or command is None:
        raise CheckFailure(f"{path}:{line}: registered name {name!r} has no tests/{name}.cpp")
    program, raw_index = sliced.groups()
    source = ROOT / "tests" / f"{program}.cpp"
    if not source.is_file():
        raise CheckFailure(
            f"{path}:{line}: slice program {program!r} has no tests/{program}.cpp")
    try:
        count = _slice_count(source)
    except CheckFailure as error:
        raise CheckFailure(f"{path}:{line}: {error}") from error
    index = int(raw_index)
    if not 1 <= index <= count:
        raise CheckFailure(f"{path}:{line}: slice index {index} is outside 1..{count}")
    if raw_index != str(index):
        raise CheckFailure(f"{path}:{line}: slice index {raw_index!r} is not canonical")
    expected = [program, "--slice", f"{index}/{count}"]
    if command != expected:
        raise CheckFailure(f"{path}:{line}: slice command must be {' '.join(expected)}")


def _is_script_add_test(words: list[str]) -> bool:
    command = " ".join(words)
    return ("Python3_EXECUTABLE" in command or
            bool(re.search(r"\.(?:py|sh|cmake)(?:\s|$)", command)) or
            ("CMAKE_COMMAND" in command and " -P " in f" {command} "))


def _check_skip_values(path: Path, statement: Statement, pairs: list[str]) -> None:
    for key, value in zip(pairs[::2], pairs[1::2]):
        if key.upper() == "SKIP_RETURN_CODE" and value != "77":
            raise CheckFailure(f"{path}:{statement.line}: SKIP_RETURN_CODE must be 77")


def _check_lane(path: Path, owners: dict[str, dict[str, set[str]]]) -> None:
    try:
        text = path.read_text()
    except OSError as error:
        raise CheckFailure(f"{path}: cannot read: {error}") from error
    statements = _statements(path, text)
    depth = 0
    for statement in statements:
        name = statement.name.lower()
        words = _words(statement.body)
        if name == "if":
            depth += 1
            continue
        if name == "endif":
            if depth == 0:
                raise CheckFailure(f"{path}:{statement.line}: endif() without if()")
            depth -= 1
            continue
        if name == "list":
            if len(words) < 3 or words[0].upper() != "APPEND" or words[1] != "TEST_SOURCES":
                raise CheckFailure(f"{path}:{statement.line}: only list(APPEND TEST_SOURCES ...) is allowed")
            for item in words[2:]:
                _require_source(path, statement.line, item)
            continue
        if name == "pineforge_add_unit_test":
            if len(words) != 1:
                raise CheckFailure(f"{path}:{statement.line}: pineforge_add_unit_test() needs one test name")
            _require_source(path, statement.line, words[0])
            continue
        if name == "add_test":
            if len(words) < 3 or words[0].upper() != "NAME":
                raise CheckFailure(f"{path}:{statement.line}: add_test() needs NAME and COMMAND")
            if "COMMAND" not in (word.upper() for word in words):
                raise CheckFailure(f"{path}:{statement.line}: add_test() needs COMMAND")
            if not _is_script_add_test(words):
                command_at = next(index for index, word in enumerate(words)
                                  if word.upper() == "COMMAND")
                _require_source(path, statement.line, words[1], words[command_at + 1:])
            continue
        if name == "set_tests_properties":
            words = _property_words(path, statement)
            if "PROPERTIES" not in (word.upper() for word in words):
                raise CheckFailure(f"{path}:{statement.line}: {statement.name}() needs PROPERTIES")
            properties = next(index for index, word in enumerate(words)
                              if word.upper() == "PROPERTIES")
            if properties == 0:
                raise CheckFailure(f"{path}:{statement.line}: {statement.name}() needs a registered name")
            pairs = words[properties + 1:]
            if not pairs:
                raise CheckFailure(f"{path}:{statement.line}: set_tests_properties() needs at least one key/value pair")
            if len(pairs) % 2:
                raise CheckFailure(f"{path}:{statement.line}: set_tests_properties() needs complete key/value pairs")
            for value in words[:properties]:
                _owned(statement, path.stem, "test", value, owners, path=path)
            _check_skip_values(path, statement, pairs)
            for key in pairs[::2]:
                canonical = key.upper()
                if canonical in REFUSED_TEST_PROPERTIES:
                    raise CheckFailure(f"{path}:{statement.line}: property {canonical} is explicitly forbidden in a lane file")
                if canonical not in ALLOWED_TEST_PROPERTIES:
                    raise CheckFailure(f"{path}:{statement.line}: property {canonical!r} is not allowed in a lane file")
            continue
        raise CheckFailure(f"{path}:{statement.line}: statement {statement.name}() is not allowed in a lane file")
    if depth:
        raise CheckFailure(f"{path}: missing endif() for {depth} if() block(s)")


def _fence_markers(text: str) -> list[FenceMarker]:
    markers: list[FenceMarker] = []
    for match in FENCE_COMMENT_OR_ARGUMENT_RE.finditer(text):
        marker = re.fullmatch(
            r"# --- CR-150 (\S+) (begin|end) ---[ \t]*", match.group(0))
        if marker is None:
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        if text[line_start:match.start()].strip():
            continue
        markers.append(FenceMarker(
            marker.group(1), marker.group(2), match.start(), match.end(),
            text.count("\n", 0, match.start()) + 1))
    return markers


def _no_unfenced_statements(text: str, start: int, end: int) -> None:
    if _without_comments(text[start:end], bracket_comments=True).strip():
        line = text.count("\n", 0, start) + 1
        raise CheckFailure(
            f"{CMAKE}:{line}: unfenced statement after the CR-150 include block")


def _fences(text: str, markers: list[FenceMarker]) -> list[Fence]:
    fences: list[Fence] = []
    seen: set[str] = set()
    active: FenceMarker | None = None
    previous_end = markers[0].start
    lane_names = {path.stem for path in LANE_DIR.glob("*.cmake") if path.is_file()}
    for marker in markers:
        if marker.edge == "begin":
            if active is not None:
                raise CheckFailure(f"{CMAKE}:{marker.line}: nested CR-150 fence")
            _no_unfenced_statements(text, previous_end, marker.start)
            if marker.lane in seen:
                raise CheckFailure(f"{CMAKE}:{marker.line}: duplicate CR-150 fence for {marker.lane}")
            if marker.lane not in lane_names:
                raise CheckFailure(
                    f"{CMAKE}:{marker.line}: fence has no lane file tests/cr150.d/{marker.lane}.cmake")
            seen.add(marker.lane)
            active = marker
        else:
            if active is None:
                raise CheckFailure(f"{CMAKE}:{marker.line}: end fence without begin fence")
            if marker.lane != active.lane:
                raise CheckFailure(
                    f"{CMAKE}:{marker.line}: end fence {marker.lane} does not match begin fence {active.lane}")
            # Pad with newlines so every diagnostic retains its real line in
            # tests/CMakeLists.txt; markers are never part of a statement.
            body = "\n" * text.count("\n", 0, active.end) + text[active.end:marker.start]
            fences.append(Fence(
                active.lane, _statements(CMAKE, body, mask_quotes=True), active.line))
            previous_end = marker.end
            active = None
    if active is not None:
        raise CheckFailure(f"{CMAKE}:{active.line}: missing end fence for {active.lane}")
    tail_code = " ".join(_without_comments(text[previous_end:], bracket_comments=True).split())
    fast_code = " ".join(_without_comments(FAST_TEST_REGISTRATION, bracket_comments=True).split())
    if tail_code != fast_code:
        _no_unfenced_statements(text, previous_end, len(text))
    return fences


def _check_include_loop(statements: list[Statement], fenced: bool) -> None:
    if len(statements) < 5:
        if fenced:
            raise CheckFailure(f"{CMAKE}: fence must follow the CR-150 include block")
        raise CheckFailure(f"{CMAKE}: CR-150 include block is missing")
    tail = statements[-5:]
    expected = ["file", "list", "foreach", "include", "endforeach"]
    if [statement.name.lower() for statement in tail] != expected:
        line = statements[-1].line if statements else 1
        if fenced and not any(statement.name.lower() == "file" and
                              "CR150_LANE_CMAKE" in _words(statement.body)
                              for statement in statements):
            raise CheckFailure(f"{CMAKE}:{line}: fence must follow the CR-150 include block")
        raise CheckFailure(
            f"{CMAKE}:{line}: CR-150 include block is not the last statement; "
            "unfenced statement after the CR-150 include block")
    file_body, list_body, foreach_body, include_body, _ = [statement.body for statement in tail]
    file_words = _words(file_body)
    if (len(file_words) != 4 or file_words[:3] != ["GLOB", "CR150_LANE_CMAKE", "CONFIGURE_DEPENDS"] or
            file_words[3] != "${CMAKE_CURRENT_SOURCE_DIR}/cr150.d/*.cmake"):
        raise CheckFailure(f"{CMAKE}:{tail[0].line}: file(GLOB ...) does not use CONFIGURE_DEPENDS for cr150.d/*.cmake")
    if _words(list_body) != ["SORT", "CR150_LANE_CMAKE"]:
        raise CheckFailure(f"{CMAKE}:{tail[1].line}: CR150_LANE_CMAKE is not sorted")
    if _words(foreach_body) != ["cr150_lane_file", "IN", "LISTS", "CR150_LANE_CMAKE"]:
        raise CheckFailure(f"{CMAKE}:{tail[2].line}: include loop does not use CR150_LANE_CMAKE")
    if _words(include_body) != ["${cr150_lane_file}"]:
        raise CheckFailure(f"{CMAKE}:{tail[3].line}: include loop does not include each lane file")


def _declarations(statement: Statement, words: list[str]) -> list[tuple[str, str]]:
    """Return actual creators, not list inventory entries or property uses."""
    name = statement.name.lower()
    if name == "pineforge_add_unit_test" and len(words) == 1:
        return [("target", words[0]), ("test", words[0])]
    if name in ("add_library", "add_executable") and words:
        return [("target", words[0])]
    if name == "add_test" and len(words) >= 2 and words[0].upper() == "NAME":
        return [("test", words[1])]
    return []


def _ownership(prefix: list[Statement], fences: list[Fence]) -> dict[str, dict[str, set[str]]]:
    owners: dict[str, dict[str, set[str]]] = {"target": {}, "test": {}}

    def record(kind: str, value: str, lane: str) -> None:
        if LITERAL_NAME_RE.fullmatch(value):
            owners[kind].setdefault(value, set()).add(lane)

    # The existing main-file inventory also creates targets/tests through its
    # earlier registration loop; do not let a fence re-declare those names.
    for statement in prefix:
        words = _words(statement.body)
        name = statement.name.lower()
        if name in ("add_library", "add_executable", "pineforge_add_unit_test", "add_test"):
            limit = 2 if name == "add_test" else 1
            for kind, value in _declarations(statement, _fence_words(statement, limit=limit)):
                record(kind, value, "<main CMakeLists.txt>")
        inventory: list[str] = []
        if name == "set" and words and words[0] == "TEST_SOURCES":
            inventory = words[1:]
        if name == "list" and words[:2] == ["APPEND", "TEST_SOURCES"]:
            inventory = words[2:]
        for value in inventory:
            record("target", value, "<main CMakeLists.txt>")
            record("test", value, "<main CMakeLists.txt>")
    for path in sorted(LANE_DIR.glob("*.cmake")):
        try:
            statements = _statements(path, path.read_text())
        except OSError as error:
            raise CheckFailure(f"{path}: cannot read: {error}") from error
        for statement in statements:
            if statement.name.lower() in ("pineforge_add_unit_test", "add_test"):
                # Discover just NAME/name. Do not impose the fence argument
                # policy on an existing lane file's command payload.
                limit = 2 if statement.name.lower() == "add_test" else 1
                for kind, value in _declarations(statement, _fence_words(statement, limit=limit)):
                    record(kind, value, path.stem)
    # Discover every fence before checking writes, so changing fence order
    # cannot conceal a second lane's declaration.
    for fence in fences:
        for statement in fence.statements:
            for kind, value in _declarations(statement, _fence_words(statement)):
                if not LITERAL_NAME_RE.fullmatch(value):
                    raise CheckFailure(
                        f"{CMAKE}:{statement.line}: {kind} name {value!r} is not a literal")
                previous = owners[kind].get(value, set())
                if previous:
                    raise CheckFailure(
                        f"{CMAKE}:{statement.line}: {kind} {value!r} is created by "
                        f"{', '.join(sorted(previous))}; fence {fence.lane} cannot re-declare it")
                record(kind, value, fence.lane)
    return owners


def _owned(statement: Statement, lane: str, kind: str, value: str,
           owners: dict[str, dict[str, set[str]]], *, path: Path | None = None) -> None:
    if not LITERAL_NAME_RE.fullmatch(value) or owners[kind].get(value) != {lane}:
        owner = f"lane file {lane}" if path is not None else f"fence {lane}"
        raise CheckFailure(
            f"{path if path is not None else CMAKE}:{statement.line}: "
            f"{kind} {value!r} is not owned by {owner}")


def _check_reads(statement: Statement, lane: str, words: list[str],
                 owners: dict[str, dict[str, set[str]]], *, condition: bool = False) -> None:
    external = {value for value, creators in owners["target"].items()
                if lane not in creators and creators != {"<main CMakeLists.txt>"}}

    def refused_expression() -> None:
        raise CheckFailure(
            f"{CMAKE}:{statement.line}: only literal $<TARGET_FILE:...> reads are allowed "
            "for external targets")

    def mask_expressions(value: str) -> str:
        """Balance nested generator expressions; erase permitted file reads."""
        result: list[str] = []
        cursor = 0
        while (begin := value.find("$<", cursor)) != -1:
            result.append(value[cursor:begin])
            index = begin + 2
            depth = 1
            while index < len(value) and depth:
                if value.startswith("$<", index):
                    depth += 1
                    index += 2
                elif value[index] == ">":
                    depth -= 1
                    index += 1
                else:
                    index += 1
            if depth:
                raise CheckFailure(f"{CMAKE}:{statement.line}: unterminated generator expression")
            body = value[begin + 2:index - 1]
            # Find the operator's separator outside any nested expression.
            # In $<$<CONFIG:Debug>:...>, the first ':' belongs to CONFIG.
            split_at = 0
            nested = 0
            while split_at < len(body):
                if body.startswith("$<", split_at):
                    nested += 1
                    split_at += 2
                    continue
                if body[split_at] == ">":
                    nested -= 1
                if body[split_at] == ":" and nested == 0:
                    break
                split_at += 1
            operator = body[:split_at]
            separator = split_at < len(body)
            arguments = body[split_at + 1:] if separator else ""
            if any(expansion in operator for expansion in ("${", "$ENV{", "$CACHE{")):
                refused_expression()
            if operator == "TARGET_FILE":
                if not separator or not LITERAL_NAME_RE.fullmatch(arguments):
                    refused_expression()
            else:
                if operator.startswith("TARGET_"):
                    # A target-property expression with one argument reads a
                    # property of the current target. All explicit target
                    # operands in other forms must name this lane's target.
                    implicit = operator in ("TARGET_PROPERTY", "TARGET_POLICY") and "," not in arguments
                    target = arguments.split(",", 1)[0]
                    if ("$" in arguments or not separator or
                            (not implicit and owners["target"].get(target) != {lane})):
                        refused_expression()
                masked_body = mask_expressions(body)
                if any(part in external for part in re.split(r"[:,;<>]", masked_body)):
                    raise CheckFailure(
                        f"{CMAKE}:{statement.line}: external target reads require "
                        "$<TARGET_FILE:...> or if(TARGET ...)")
                result.append("$<" + masked_body + ">")
            cursor = index
        result.append(value[cursor:])
        return "".join(result)

    for index, word in enumerate(words):
        if condition and word.upper() == "TARGET":
            if index + 1 == len(words) or not LITERAL_NAME_RE.fullmatch(words[index + 1]):
                raise CheckFailure(f"{CMAKE}:{statement.line}: if(TARGET ...) needs a literal target")
        masked = mask_expressions(word)
        if any(part in external for part in masked.split(";")):
            if condition and index and words[index - 1].upper() == "TARGET":
                continue
            raise CheckFailure(
                f"{CMAKE}:{statement.line}: external target reads require "
                "$<TARGET_FILE:...> or if(TARGET ...)")


def _check_fence(fence: Fence, owners: dict[str, dict[str, set[str]]]) -> None:
    branches: list[bool] = []  # whether an if() block has already reached else()
    for statement in fence.statements:
        name = statement.name.lower()
        words = _fence_words(statement)
        reads: list[str] = []
        if name in ("if", "elseif", "else", "endif"):
            if name == "if":
                branches.append(False)
            else:
                if not branches:
                    raise CheckFailure(f"{CMAKE}:{statement.line}: {name}() without if()")
                if name in ("else", "elseif") and branches[-1]:
                    raise CheckFailure(f"{CMAKE}:{statement.line}: {name}() after else()")
                if name == "else":
                    branches[-1] = True
                elif name == "endif":
                    branches.pop()
            if name in ("if", "elseif"):
                if not words:
                    raise CheckFailure(f"{CMAKE}:{statement.line}: {name}() needs a condition")
                _check_reads(statement, fence.lane, words, owners, condition=True)
            elif words:
                raise CheckFailure(f"{CMAKE}:{statement.line}: {name}() must have no arguments in a fence")
            continue
        if name == "list":
            if len(words) < 3 or words[:2] != ["APPEND", "TEST_SOURCES"]:
                raise CheckFailure(
                    f"{CMAKE}:{statement.line}: only list(APPEND TEST_SOURCES ...) is allowed in a fence")
            for value in words[2:]:
                _require_source(CMAKE, statement.line, value)
                _owned(statement, fence.lane, "target", value, owners)
        elif name == "pineforge_add_unit_test":
            if len(words) != 1:
                raise CheckFailure(
                    f"{CMAKE}:{statement.line}: pineforge_add_unit_test() needs one test name")
            _require_source(CMAKE, statement.line, words[0])
            _owned(statement, fence.lane, "target", words[0], owners)
            _owned(statement, fence.lane, "test", words[0], owners)
        elif name in ("add_library", "add_executable"):
            if any(word.upper() in ("ALIAS", "IMPORTED") for word in words[1:]):
                raise CheckFailure(
                    f"{CMAKE}:{statement.line}: ALIAS and IMPORTED targets are not allowed in a fence")
            if not words:
                raise CheckFailure(f"{CMAKE}:{statement.line}: {name}() needs a literal target")
            flags = ({"STATIC", "SHARED", "MODULE", "OBJECT", "INTERFACE", "EXCLUDE_FROM_ALL"}
                     if name == "add_library" else {"WIN32", "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL"})
            for word in words[1:]:
                # Until the first source, an expansion could turn into ALIAS
                # or IMPORTED, which does not create a lane-owned target.
                if any(expansion in word for expansion in ("${", "$ENV{", "$CACHE{")):
                    raise CheckFailure(
                        f"{CMAKE}:{statement.line}: target creation options must be literal; "
                        "expanded sources can be added with target_sources()")
                if word.upper() not in flags:
                    break
            _owned(statement, fence.lane, "target", words[0], owners)
            reads = words[1:]
        elif name == "add_test":
            if len(words) < 4 or words[0].upper() != "NAME" or words[2].upper() != "COMMAND":
                raise CheckFailure(f"{CMAKE}:{statement.line}: add_test() needs NAME and COMMAND")
            _owned(statement, fence.lane, "test", words[1], owners)
            reads = words[3:]
        elif name in TARGET_COMMANDS:
            scope = 1
            flags = {"BEFORE", "AFTER", "SYSTEM"} if name == "target_include_directories" else {"BEFORE"}
            while scope < len(words) and words[scope].upper() in flags:
                scope += 1
            if (len(words) <= scope + 1 or not LITERAL_NAME_RE.fullmatch(words[0]) or
                    words[scope].upper() not in ("PUBLIC", "PRIVATE", "INTERFACE")):
                raise CheckFailure(
                    f"{CMAKE}:{statement.line}: {name}() needs a literal target and a scope")
            _owned(statement, fence.lane, "target", words[0], owners)
            reads = words[1:]
        elif name in ("set_target_properties", "set_tests_properties"):
            properties = next((index for index, word in enumerate(words)
                               if word.upper() == "PROPERTIES"), 0)
            pairs = words[properties + 1:]
            if not properties or not pairs or len(pairs) % 2:
                raise CheckFailure(
                    f"{CMAKE}:{statement.line}: {name}() needs names and PROPERTIES key/value pairs")
            kind = "target" if name == "set_target_properties" else "test"
            for value in words[:properties]:
                _owned(statement, fence.lane, kind, value, owners)
            if kind == "test":
                _check_skip_values(CMAKE, statement, pairs)
            reads = pairs[1::2]
        elif name == "add_dependencies":
            if len(words) < 2:
                raise CheckFailure(f"{CMAKE}:{statement.line}: add_dependencies() needs a target and dependencies")
            _owned(statement, fence.lane, "target", words[0], owners)
            reads = words[1:]
        elif name == "set_property":
            if not words or words[0].upper() != "TEST":
                raise CheckFailure(
                    f"{CMAKE}:{statement.line}: only set_property(TEST ...) is allowed in a fence")
            property_at = next((index for index, word in enumerate(words)
                                if word.upper() == "PROPERTY"), 0)
            tests = words[1:property_at]
            if tests and tests[-1].upper() in ("APPEND", "APPEND_STRING"):
                tests = tests[:-1]
            if not tests or property_at + 1 >= len(words):
                raise CheckFailure(f"{CMAKE}:{statement.line}: set_property(TEST ...) needs tests and PROPERTY")
            for value in tests:
                _owned(statement, fence.lane, "test", value, owners)
            reads = words[property_at + 2:]
            _check_skip_values(CMAKE, statement,
                               [words[property_at + 1], ";".join(reads)])
        else:
            raise CheckFailure(
                f"{CMAKE}:{statement.line}: statement {statement.name}() is not allowed in a fence")
        _check_reads(statement, fence.lane, reads, owners)
    if branches:
        raise CheckFailure(f"{CMAKE}:{fence.line}: missing endif() inside fence {fence.lane}")


def _without_final_fast_validation(text: str) -> str:
    start = text.rfind("foreach(_pf_fast_test IN LISTS PINEFORGE_PR_FAST_TESTS)")
    if start < 0:
        return text
    def code(value: str) -> list[str]:
        return [line.strip() for line in _without_comments(
            value, bracket_comments=True).splitlines() if line.strip()]
    if code(text[start:]) == code(FINAL_FAST_VALIDATION):
        return text[:start]
    return text


def _profile_truth(words: list[str], profile: tuple[bool, bool]) -> set[bool]:
    """Possible condition values; unknown CMake predicates can be true or false."""
    depth = 0
    start = 0
    operands: list[list[str]] = []
    operators: list[str] = []
    for index, word in enumerate(words):
        if word == "(":
            depth += 1
        elif word == ")":
            depth -= 1
        elif depth == 0 and word.upper() in ("AND", "OR"):
            operands.append(words[start:index])
            operators.append(word.upper())
            start = index + 1
    if operators:
        operands.append(words[start:])
        left = _profile_truth(operands[0], profile)
        # CMake gives AND and OR equal precedence and folds left to right.
        # Evaluate every operand; NOT and parenthesized groups recurse first.
        for operator, operand in zip(operators, operands[1:]):
            right = _profile_truth(operand, profile)
            left = {(a or b) if operator == "OR" else (a and b)
                    for a in left for b in right}
        return left
    if words and words[0].upper() == "NOT":
        return {not value for value in _profile_truth(words[1:], profile)}
    if words and words[0] == "(" and words[-1] == ")":
        return _profile_truth(words[1:-1], profile)
    if words == ["PINEFORGE_ENABLE_SANITIZERS"]:
        return {profile[0]}
    if words == ["PINEFORGE_ENABLE_COVERAGE"]:
        return {profile[1]}
    if len(words) == 1:
        if words[0].upper() in ("1", "ON", "TRUE", "YES"):
            return {True}
        if words[0].upper() in ("0", "OFF", "FALSE", "NO"):
            return {False}
    return {False, True}


def _profile_statements(path: Path, statements: list[Statement]):
    """Track feasible sanitizer/coverage settings separately in every branch."""
    profiles = {(False, False), (False, True), (True, False), (True, True)}
    branches: list[tuple[set[tuple[bool, bool]], set[tuple[bool, bool]]]] = []
    for statement in statements:
        name = statement.name.lower()
        if name in ("if", "elseif", "else", "endif"):
            # Preserve quotes: a quoted option name is not a variable under
            # CMP0054 and cannot prove a profile exclusion.
            words = re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+', statement.body)
            if name == "if":
                remaining = {p for p in profiles if False in _profile_truth(words, p)}
                branches.append((profiles, remaining))
                profiles = {p for p in profiles if True in _profile_truth(words, p)}
            elif not branches:
                raise CheckFailure(f"{path}:{statement.line}: {name}() without if()")
            elif name == "endif":
                profiles, _ = branches.pop()
            else:
                outer, remaining = branches[-1]
                profiles = (remaining if name == "else" else
                            {p for p in remaining if True in _profile_truth(words, p)})
                branches[-1] = (
                    outer, set() if name == "else" else
                    {p for p in remaining if False in _profile_truth(words, p)})
            continue
        if name in ("add_library", "add_test", "set_tests_properties", "set_property"):
            yield statement, _fence_words(statement), profiles
    if branches:
        raise CheckFailure(f"{path}: missing endif() in profile guard")


def _check_python_modules(prefix: list[Statement], fences: list[Fence]) -> None:
    """H2-3: join MODULE declarations, Python rows and their TARGET_FILE reads."""
    inputs = [(CMAKE, prefix)]
    inputs.extend((CMAKE, fence.statements) for fence in fences)
    inputs.extend((path, _statements(path, path.read_text()))
                  for path in sorted(LANE_DIR.glob("*.cmake")))
    modules: dict[str, set[tuple[bool, bool]]] = {}
    rows: dict[str, set[tuple[bool, bool]]] = {}
    reads: list[tuple[Path, Statement, str, str, set[tuple[bool, bool]]]] = []
    for path, statements in inputs:
        for statement, words, profiles in _profile_statements(path, statements):
            name = statement.name.lower()
            if (name == "add_library" and len(words) >= 2 and
                    LITERAL_NAME_RE.fullmatch(words[0])):
                # EXCLUDE_FROM_ALL may precede the type. Stop at the first
                # other word so a source named MODULE cannot change the type.
                library_type = next((word.upper() for word in words[1:]
                                     if word.upper() != "EXCLUDE_FROM_ALL"), "")
                if library_type == "MODULE":
                    modules.setdefault(words[0], set()).update(profiles)
            tests: list[str] = []
            values: list[str] = []
            commands: list[list[str]] = []
            if name == "add_test" and len(words) >= 4 and words[0].upper() == "NAME":
                command = " ".join(words[3:])
                if ("Python3_EXECUTABLE" in command or
                        re.search(r"(?:^|[/\s])(?:python[0-9.]*|pytest)(?:\s|$)|\.py(?:\s|$)",
                                  command)):
                    rows.setdefault(words[1], set()).update(profiles)
                tests, values = [words[1]], words[3:]
                commands = [values]
            elif name == "set_tests_properties":
                at = next((i for i, word in enumerate(words)
                           if word.upper() == "PROPERTIES"), len(words))
                tests, values = words[:at], words[at + 2::2]
                commands = [[value] for key, value in zip(words[at + 1::2], values)
                            if key.upper() == "COMMAND"]
            elif name == "set_property" and words and words[0].upper() == "TEST":
                at = next((i for i, word in enumerate(words)
                           if word.upper() == "PROPERTY"), len(words))
                tests = [word for word in words[1:at]
                         if word.upper() not in ("APPEND", "APPEND_STRING")]
                values = words[at + 2:]
                if at + 1 < len(words) and words[at + 1].upper() == "COMMAND":
                    commands = [values]
            # Main-file command lists include the existing ASan preload row.
            # Lanes/fences must expose commands instead of hiding target reads.
            if path != CMAKE or statements is not prefix:
                for command_words in commands:
                    if (len(command_words) == 1 and
                            re.fullmatch(r"\$\{[^{}]+\}", command_words[0])):
                        raise CheckFailure(
                            f"{path}:{statement.line}: bare COMMAND variable cannot be checked; "
                            "write out the command in the lane or fence")
            for file_target, property_target in re.findall(
                    r"\$<TARGET_(?:(?:FILE|LINKER_FILE|SONAME_FILE)(?:_DIR|_NAME)?:"
                    r"([A-Za-z0-9_.+-]+)>|PROPERTY:([A-Za-z0-9_.+-]+),)",
                    " ".join(values)):
                target = file_target or property_target
                for test in tests:
                    reads.append((path, statement, test, target, profiles))
    for path, statement, test, target, profiles in reads:
        feasible = profiles & rows.get(test, set()) & modules.get(target, set())
        if feasible - {(False, False)}:
            raise CheckFailure(
                f"{path}:{statement.line}: MODULE {target} reaches Python row {test}; "
                "require NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE "
                "on the module or row")


def _check_registration_timeouts() -> None:
    """H2-8: no lane/fence or new main-file row may declare TIMEOUT."""
    # This exact statement predates the CR-150 registrations. Preserve the
    # inherited ownership rows, without granting a deadline to any new row.
    inherited_main = [
        "test_selected_window_ownership_manifest", "${_pf_ownership_control_shards}",
        "test_selected_window_ownership_d4", "PROPERTIES", "WORKING_DIRECTORY",
        "${PROJECT_SOURCE_DIR}", "TIMEOUT", "180",
    ]
    for path in [CMAKE, *sorted(LANE_DIR.glob("*.cmake"))]:
        for statement in _statements(path, path.read_text(), mask_quotes=True):
            name = statement.name.lower()
            if name not in ("set_tests_properties", "set_property"):
                continue
            words = _fence_words(statement)
            if path == CMAKE and words == inherited_main:
                continue
            if name == "set_tests_properties":
                at = next((i for i, word in enumerate(words)
                           if word.upper() == "PROPERTIES"), len(words))
                keys = words[at + 1::2]
            elif words and words[0].upper() == "TEST":
                at = next((i for i, word in enumerate(words)
                           if word.upper() == "PROPERTY"), len(words))
                keys = words[at + 1:at + 2]
            else:
                continue
            if any(key.upper() == "TIMEOUT" for key in keys):
                raise CheckFailure(
                    f"{path}:{statement.line}: TIMEOUT is forbidden (owner #827)")


def _check_cmake_tail() -> dict[str, dict[str, set[str]]]:
    try:
        text = CMAKE.read_text()
    except OSError as error:
        raise CheckFailure(f"{CMAKE}: cannot read: {error}") from error
    text = _without_final_fast_validation(text)
    markers = _fence_markers(text)
    prefix = _statements(CMAKE, text[:markers[0].start] if markers else text)
    _check_include_loop(prefix, bool(markers))
    fences = _fences(text, markers) if markers else []
    owners = _ownership(prefix[:-5], fences)
    for fence in fences:
        _check_fence(fence, owners)
    _check_python_modules(prefix[:-5], fences)
    return owners


def main() -> int:
    try:
        owners = _check_cmake_tail()
        if not README.is_file():
            raise CheckFailure(f"{README}: README.md is missing")
        if not LANE_DIR.is_dir():
            raise CheckFailure(f"{LANE_DIR}: lane directory is missing")
        for path in sorted(LANE_DIR.glob("*.cmake")):
            _check_lane(path, owners)
        _check_registration_timeouts()
    except CheckFailure as error:
        print(f"check_cr150_cmake_d: {error}", file=sys.stderr)
        return 1
    print("check_cr150_cmake_d: CR-150 CMake lane directory is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
