#!/usr/bin/env python3
"""Check the CR-150 per-lane CMake registration directory."""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CMAKE = ROOT / "tests" / "CMakeLists.txt"
LANE_DIR = ROOT / "tests" / "cr150.d"
README = LANE_DIR / "README.md"


class CheckFailure(Exception):
    pass


@dataclass(frozen=True)
class Statement:
    name: str
    body: str
    line: int


def _without_comments(text: str) -> str:
    """Mask comments while preserving newlines and quoted arguments."""
    lines: list[str] = []
    for raw in text.splitlines(keepends=True):
        quoted = False
        escaped = False
        out: list[str] = []
        for char in raw:
            if escaped:
                escaped = False
                out.append(char)
                continue
            if char == "\\" and quoted:
                escaped = True
                out.append(char)
                continue
            if char == '"':
                quoted = not quoted
                out.append(char)
                continue
            if char == "#" and not quoted:
                out.append("\n" if raw.endswith("\n") else "")
                break
            out.append(char)
        lines.append("".join(out))
    return "".join(lines)


def _statements(path: Path, text: str) -> list[Statement]:
    """Read balanced, line-started CMake calls with their source lines."""
    masked = _without_comments(text)
    starts = list(re.finditer(r"(?m)^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(", masked))
    statements: list[Statement] = []
    cursor = 0
    for match in starts:
        between = masked[cursor:match.start()]
        if between.strip():
            line = masked.count("\n", 0, match.start()) + 1
            raise CheckFailure(f"{path}:{line}: text is not a CMake statement")
        open_at = match.end() - 1
        depth = 0
        quoted = False
        escaped = False
        close_at: int | None = None
        for index in range(open_at, len(masked)):
            char = masked[index]
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
        line = masked.count("\n", 0, match.start()) + 1
        statements.append(Statement(match.group(1), body, line))
        cursor = close_at + 1
    if masked[cursor:].strip():
        line = masked.count("\n", 0, cursor) + 1
        raise CheckFailure(f"{path}:{line}: text is not a CMake statement")
    return statements


def _words(body: str) -> list[str]:
    return [token[1:-1] if len(token) >= 2 and token[0] == token[-1] == '"' else token
            for token in re.findall(r'"(?:\\.|[^"\\])*"|[^\s]+', body)]


def _require_source(path: Path, line: int, name: str) -> None:
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
        raise CheckFailure(f"{path}:{line}: registered name {name!r} is not a literal")
    source = ROOT / "tests" / f"{name}.cpp"
    if not source.is_file():
        raise CheckFailure(f"{path}:{line}: registered name {name!r} has no tests/{name}.cpp")


def _is_script_add_test(words: list[str]) -> bool:
    command = " ".join(words)
    return ("Python3_EXECUTABLE" in command or
            bool(re.search(r"\.(?:py|sh|cmake)(?:\s|$)", command)) or
            ("CMAKE_COMMAND" in command and " -P " in f" {command} "))


def _check_lane(path: Path) -> None:
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
                _require_source(path, statement.line, words[1])
            continue
        if name in {"set_tests_properties", "set_target_properties"}:
            if "PROPERTIES" not in (word.upper() for word in words):
                raise CheckFailure(f"{path}:{statement.line}: {statement.name}() needs PROPERTIES")
            if words[0].upper() == "PROPERTIES":
                raise CheckFailure(f"{path}:{statement.line}: {statement.name}() needs a registered name")
            continue
        raise CheckFailure(f"{path}:{statement.line}: statement {statement.name}() is not allowed in a lane file")
    if depth:
        raise CheckFailure(f"{path}: missing endif() for {depth} if() block(s)")


def _check_cmake_tail() -> None:
    try:
        text = CMAKE.read_text()
    except OSError as error:
        raise CheckFailure(f"{CMAKE}: cannot read: {error}") from error
    statements = _statements(CMAKE, text)
    if len(statements) < 5:
        raise CheckFailure(f"{CMAKE}: CR-150 include block is missing")
    tail = statements[-5:]
    expected = ["file", "list", "foreach", "include", "endforeach"]
    if [statement.name.lower() for statement in tail] != expected:
        line = statements[-1].line if statements else 1
        raise CheckFailure(f"{CMAKE}:{line}: CR-150 include block is not the last statement")
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


def main() -> int:
    try:
        _check_cmake_tail()
        if not README.is_file():
            raise CheckFailure(f"{README}: README.md is missing")
        if not LANE_DIR.is_dir():
            raise CheckFailure(f"{LANE_DIR}: lane directory is missing")
        for path in sorted(LANE_DIR.glob("*.cmake")):
            _check_lane(path)
    except CheckFailure as error:
        print(f"check_cr150_cmake_d: {error}", file=sys.stderr)
        return 1
    print("check_cr150_cmake_d: CR-150 CMake lane directory is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
