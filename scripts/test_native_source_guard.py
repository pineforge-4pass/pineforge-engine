#!/usr/bin/env python3
"""Guard native source boundaries, member definitions and sync portability.

This is the inexpensive textual companion to check_native_include_independence:
the latter proves the installed closure and object symbols after a build; this
guard runs in preflight and rejects a forbidden include before compilation.
"""
from __future__ import annotations

from bisect import bisect_right
from contextlib import redirect_stderr
import io
from pathlib import Path
import re
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
HEADER_ROOTS = (
    "include/pineforge/engine.hpp",
    "include/pineforge/native_host.hpp",
    "include/pineforge/native_order.hpp",
    "include/pineforge/native_run_spec.hpp",
    "include/pineforge/native_calendar.hpp",
    "include/pineforge/execution.hpp",
    "include/pineforge/market_driver.hpp",
)
SOURCE_ROOTS = (
    "src/engine_execution.cpp",
    "src/engine_state_hash.cpp",
    "src/c_abi.cpp",
    "src/pending_order_mirror.cpp",
)
IDENTIFIER_ROOTS = (
    "include/pineforge/engine.hpp",
    "include/pineforge/native_host.hpp",
    "src/native_execution_consumer.cpp",
    "src/native_order.cpp",
    "src/engine_execution.cpp",
    "src/c_abi.cpp",
    "src/engine_state_hash.cpp",
)
FORBIDDEN_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"][^>"]*(?:pineforge/source/|compat/pine/)'
)
FORBIDDEN_C_ABI = re.compile(
    r'(?:pineforge::source|\bsource::|compat::pine|pineforge/source/|compat/pine/)'
)
_MATCH_LOOP = "process_pending_orders"
_PENDING_ROSTER = "pending_orders_"
FORBIDDEN_IDENTIFIER = re.compile(
    r'(?:compat::pine|\bpine_[A-Za-z0-9_]*\b|\b_src_[A-Za-z0-9_]*\b|'
    r'\bcoof_[A-Za-z0-9_]*\b|\bis_first_tick_\b|'
    r'\bpos_view_freeze[A-Za-z0-9_]*\b|\b' + _MATCH_LOOP + r'\b|'
    r'\btv_money[A-Za-z0-9_]*\b|\bmarket_admission_journal_\b|'
    r'\b' + _PENDING_ROSTER + r'\b)'
)
FROZEN_C_EXPORTS = (
    "strategy_pending_orders_len",
    "strategy_pending_order_get",
)


def native_roots() -> list[Path]:
    roots = [ROOT / relative for relative in HEADER_ROOTS + SOURCE_ROOTS]
    roots.extend(sorted((ROOT / "src").glob("native_*.cpp")))
    roots.extend(sorted((ROOT / "src").glob("native_*.hpp")))
    return roots


def identifier_roots() -> list[Path]:
    return [ROOT / relative for relative in IDENTIFIER_ROOTS]


def excluded_identifier_line(line: str) -> bool:
    stripped = line.lstrip()
    return (stripped.startswith(("//", "/*", "*"))
            or any(export in line for export in FROZEN_C_EXPORTS))


_DECLARATION_NAME = re.compile(r"([A-Za-z_]\w*)\s*\(")
_DECLARATION_KEYWORDS = {
    "alignof", "and", "asm", "bitand", "bitor", "catch", "decltype",
    "delete", "do", "else", "for", "if", "noexcept", "new", "not",
    "operator", "or", "reinterpret_cast", "return", "sizeof", "static_assert",
    "static_cast", "switch", "throw", "typeid", "typeof", "while", "xor",
    "void", "bool", "char", "short", "int", "long", "float", "double",
}


def _mask_cpp(text: str) -> str:
    """Blank comments and literals while preserving offsets and newlines."""
    chars = list(text)
    length = len(text)
    index = 0

    def blank(start: int, end: int) -> None:
        for position in range(start, end):
            if chars[position] != "\n":
                chars[position] = " "

    while index < length:
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end < 0 else end
            blank(index, end)
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length - 2 if end < 0 else end
            blank(index, min(length, end + 2))
            index = min(length, end + 2)
            continue
        if text.startswith('R"', index):
            delimiter_end = text.find("(", index + 2)
            if delimiter_end >= 0:
                delimiter = text[index + 2:delimiter_end]
                marker = ")" + delimiter + '"'
                marker_end = text.find(marker, delimiter_end + 1)
                if marker_end >= 0:
                    end = marker_end + len(marker)
                    blank(index, end)
                    index = end
                    continue
        # C++17 digit separators are not opening character-literal quotes.
        if (text[index].isdigit()
                and (index == 0 or not re.match(r"\w", text[index - 1]))):
            index += 1
            while index < length and (text[index].isalnum() or text[index] in "_.'"):
                index += 1
            continue
        if text[index] in {'"', "'"}:
            quote = text[index]
            end = index + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                elif text[end] == quote:
                    end += 1
                    break
                else:
                    end += 1
            blank(index, min(length, end))
            index = min(length, end)
            continue
        index += 1
    return "".join(chars)


def _class_extent(masked: str) -> tuple[int, int] | None:
    # A forward declaration must not borrow the next class's opening brace.
    match = re.search(r"\bclass\s+BacktestEngine\b\s*(?::[^{;]*)?\{", masked)
    if not match:
        return None
    opening = masked.find("{", match.start(), match.end())
    depth = 1
    index = opening + 1
    while index < len(masked):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return opening + 1, index
        index += 1
    return None


def _member_declarations(text: str) -> list[tuple[int, str]]:
    """Return top-level, non-inline BacktestEngine function declarations."""
    masked = _mask_cpp(text)
    extent = _class_extent(masked)
    if extent is None:
        return []
    start, end = extent
    segment_start = start
    brace_depth = 0
    paren_depth = 0
    bracket_depth = 0
    declarations: list[tuple[int, str]] = []
    index = start
    while index < end:
        char = masked[index]
        if char == "{":
            brace_depth += 1
        elif char == "}":
            if brace_depth:
                brace_depth -= 1
            if brace_depth == 0:
                # A nested class/struct or an inline method was consumed in
                # full; its interior is not a member declaration segment.
                segment_start = index + 1
        elif char == "(":
            paren_depth += 1
        elif char == ")":
            if paren_depth:
                paren_depth -= 1
        elif char == "[":
            bracket_depth += 1
        elif char == "]":
            if bracket_depth:
                bracket_depth -= 1
        elif char == ";" and brace_depth == 0 and paren_depth == 0 and bracket_depth == 0:
            segment = masked[segment_start:index + 1]
            # Preprocessor lines and access labels are not function members.
            segment_for_scan = "\n".join(
                "" if line.lstrip().startswith("#") else line
                for line in segment.splitlines()
            )
            if ("(" in segment_for_scan
                    and not re.search(r"\b(?:using|typedef|friend|static_assert)\b",
                                      segment_for_scan)):
                candidates = []
                for match in _DECLARATION_NAME.finditer(segment_for_scan):
                    depth = 0
                    for nested in segment_for_scan[:match.end() - 1]:
                        if nested == "(":
                            depth += 1
                        elif nested == ")" and depth:
                            depth -= 1
                    if depth == 0 and match.group(1) not in _DECLARATION_KEYWORDS:
                        candidates.append(match)
                if candidates:
                    candidate = candidates[-1]
                    # Function-style field initializers (notably
                    # numeric_limits<T>::min()) have an '=' before the call.
                    prefix = segment_for_scan[:candidate.start()]
                    if "=" not in prefix:
                        suffix = segment_for_scan[candidate.end():]
                        # Pure virtual/default/deleted members have no
                        # out-of-line definition to require.
                        if not re.search(r"=\s*(?:0|default|delete)\s*;", suffix):
                            # Find the candidate in the original segment; the
                            # preprocessor masking above preserves line count.
                            relative = segment.find(candidate.group(0))
                            absolute = segment_start + max(0, relative)
                            line = text.count("\n", 0, absolute) + 1
                            declarations.append((line, candidate.group(1)))
            segment_start = index + 1
        index += 1
    return declarations


def _defined_member_names(root: Path) -> set[str]:
    names: set[str] = set()
    source_root = root / "src"
    if not source_root.is_dir():
        return names
    for path in source_root.rglob("*"):
        if path.suffix not in {".cpp", ".hpp"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        masked = _mask_cpp(text)
        for match in re.finditer(
                r"\bBacktestEngine\s*::\s*(~?[A-Za-z_]\w*)\s*\(", masked):
            # A qualified call can look like a definition.  Require a body
            # (or an explicitly defaulted definition) before accepting it.
            close = masked.find(")", match.end())
            if close < 0:
                continue
            tail = masked[close + 1:]
            body = re.search(r"[;{]", tail)
            if body and (tail[body.start()] == "{" or
                         re.match(r"\s*=\s*default\s*;", tail)):
                names.add(match.group(1))
    return names


def _declared_but_undefined(root: Path) -> list[tuple[int, str]]:
    header = root / "include/pineforge/engine.hpp"
    try:
        declarations = _member_declarations(
            header.read_text(encoding="utf-8", errors="replace"))
    except OSError:
        return []
    defined = _defined_member_names(root)
    return sorted((line, name) for line, name in declarations if name not in defined)


def run_scan(pattern: re.Pattern[str], paths: list[Path], label: str,
             *, exclude_line=None) -> int:
    matches: list[str] = []
    for path in paths:
        try:
            with path.open(encoding="utf-8", errors="replace") as source:
                for line_number, line in enumerate(source, 1):
                    text = line.rstrip("\n")
                    if exclude_line is not None and exclude_line(text):
                        continue
                    if pattern.search(text):
                        matches.append(f"{path}:{line_number}:{text}\n")
        except OSError as error:
            print("native source guard: scan failed while checking " + label,
                  file=sys.stderr)
            print(f"{path}: {error}", file=sys.stderr)
            return 1
    if matches:
        print("native source guard: forbidden " + label + " found", file=sys.stderr)
        sys.stderr.writelines(matches)
        return 1
    return 0


def _apple_condition(expression: str) -> set[bool]:
    """Possible truth values on Apple; unsupported syntax stays unknown."""
    tokens = re.findall(r"[A-Za-z_]\w*|\d+|&&|\|\||[!()]|\S", expression)
    position = 0

    def take() -> str:
        nonlocal position
        if position == len(tokens):
            raise ValueError("incomplete condition")
        token = tokens[position]
        position += 1
        return token

    def parse(level: int = 0) -> set[bool]:
        nonlocal position
        if level < 2:
            left = parse(level + 1)
            operator = ("||", "&&")[level]
            while position < len(tokens) and tokens[position] == operator:
                position += 1
                right = parse(level + 1)
                left = {(a or b) if level == 0 else (a and b)
                        for a in left for b in right}
            return left
        token = take()
        if token == "!":
            return {not value for value in parse(2)}
        if token == "(":
            value = parse()
            if take() != ")":
                raise ValueError("unclosed condition")
            return value
        if token == "defined":
            token = take()
            if token == "(":
                token = take()
                if take() != ")":
                    raise ValueError("unclosed defined")
            if not re.fullmatch(r"[A-Za-z_]\w*", token):
                raise ValueError("invalid defined")
        if token in {"__APPLE__", "1"}:
            return {True}
        if token in {"__linux__", "0"}:
            return {False}
        return {False, True}

    try:
        result = parse()
        return result if position == len(tokens) else {False, True}
    except ValueError:
        return {False, True}


def _apple_fdatasync(text: str) -> tuple[int, list[int]]:
    """Count lexical calls and locate those not provably excluded on Apple."""
    # Splice before masking, as C++ does, including continued // comments.
    # Keep enough offset information to report physical source line numbers.
    splices = [match.start() - 2 * index for index, match in
               enumerate(re.finditer(r"\\\n", text))]
    masked = _mask_cpp(text.replace("\\\n", ""))
    calls: dict[int, list[int]] = {}
    count = 0
    for match in re.finditer(r"::\s*fdatasync\s*\(", masked):
        logical_line = masked.count("\n", 0, match.start()) + 1
        physical_offset = match.start() + 2 * bisect_right(splices, match.start())
        calls.setdefault(logical_line, []).append(
            text.count("\n", 0, physical_offset) + 1)
        count += 1
    if not count:
        return 0, []

    # Each frame holds the parent's reachability, whether a later arm may
    # still be selected, and whether #else has already appeared.
    stack: list[tuple[bool, bool, bool]] = []
    reachable = True
    exposed: list[int] = []
    for line_number, line in enumerate(masked.splitlines(), 1):
        directive = re.match(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive:
            kind, expression = directive.groups()
            if kind in {"if", "ifdef", "ifndef"}:
                if kind != "if":
                    expression = ("!" if kind == "ifndef" else "") + (
                        "defined(" + expression.strip() + ")")
                condition = _apple_condition(expression)
                stack.append((reachable, False in condition, False))
                reachable = reachable and True in condition
            else:
                if not stack or (kind != "endif" and stack[-1][2]):
                    raise ValueError(f"line {line_number}: unexpected #{kind}")
                parent, remaining, _ = stack[-1]
                if kind == "endif":
                    reachable = parent
                    stack.pop()
                else:
                    condition = _apple_condition(expression) if kind == "elif" else {True}
                    reachable = parent and remaining and True in condition
                    stack[-1] = (parent, remaining and False in condition, kind == "else")
        if reachable:
            exposed.extend(calls.get(line_number, []))
    if stack:
        raise ValueError("unterminated preprocessor conditional")
    return count, exposed


def run_portability_scan(root: Path) -> int:
    """Read every C/C++ source/header in both roots; fail on incomplete input."""
    suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl"}
    findings: list[str] = []
    calls = 0
    path = root
    try:
        for relative in ("src", "include"):
            path = root / relative
            if not path.is_dir():
                raise OSError("missing source directory")
            pending = [path]
            files = 0
            while pending:
                path = pending.pop()
                if path.is_symlink():
                    raise OSError("refusing symbolic link in source tree")
                if path.is_dir():
                    pending.extend(sorted(path.iterdir(), reverse=True))
                    continue
                if path.suffix.lower() not in suffixes:
                    continue
                text = path.read_text(encoding="utf-8")
                files += 1
                count, exposed = _apple_fdatasync(text)
                calls += count
                findings.extend(
                    f"{path}:{line}: ::fdatasync call may be compiled on Apple; "
                    "exclude Apple and use its portable sync branch\n"
                    for line in exposed)
            if not files:
                raise OSError("no C/C++ source/header inputs")
        if not calls:
            raise ValueError("no ::fdatasync calls scanned")
    except (OSError, UnicodeError, ValueError) as error:
        print(f"native source guard: portability scan failed: {path}: {error}",
              file=sys.stderr)
        return 1
    if findings:
        print("native source guard: Apple-exposed ::fdatasync call(s) found",
              file=sys.stderr)
        sys.stderr.writelines(findings)
        return 1
    return 0


def portability_self_test() -> int:
    # Independent discriminators exercise the same scanner as the real tree;
    # no production path, call count or line number is an acceptance whitelist.
    cases = (
        ("unguarded", "::fdatasync(fd);\n", 1, [1]),
        ("Apple else", "#if defined(__APPLE__)\n::fsync(fd);\n"
         "#else\n::fdatasync(fd);\n#endif\n", 1, []),
        ("Linux else", "#if defined(__linux__)\n::fdatasync(fd);\n"
         "#else\n::fsync(fd);\n#endif\n", 1, []),
        ("unrelated Apple arm", "#ifndef __APPLE__\n::fsync(fd);\n#endif\n"
         "::fdatasync(fd);\n", 1, [4]),
        ("Apple positive", "#if defined(__APPLE__)\n::fdatasync(fd);\n#endif\n", 1, [2]),
        ("wrong nested arm", "#ifndef __APPLE__\n::fsync(fd);\n#else\n"
         "#if FEATURE\n::fdatasync(fd);\n#endif\n#endif\n", 1, [5]),
        ("outer exclusion", "#ifndef __APPLE__\n#if FEATURE\n"
         "::fdatasync(fd);\n#endif\n#endif\n", 1, []),
        ("elif exclusion", "#if FEATURE\n::fsync(fd);\n"
         "#elif defined(__linux__)\n::fdatasync(fd);\n"
         "#elif defined(__APPLE__)\n::fsync(fd);\n#endif\n", 1, []),
        ("elif Apple", "#if defined(__linux__)\n::fsync(fd);\n"
         "#elif defined(__APPLE__)\n::fdatasync(fd);\n#endif\n", 1, [4]),
        ("unknown else", "#if FEATURE\n::fsync(fd);\n#else\n"
         "::fdatasync(fd);\n#endif\n", 1, [4]),
        ("boolean exclusion", "#if !defined(__APPLE__) && FEATURE\n"
         "::fdatasync(fd);\n#endif\n", 1, []),
        ("boolean exposure", "#if !defined(__APPLE__) || FEATURE\n"
         "::fdatasync(fd);\n#endif\n", 1, [2]),
        ("unsupported condition", "#if __APPLE__ == 1\n::fdatasync(fd);\n#endif\n", 1, [2]),
        ("comments and strings", "// #ifndef __APPLE__\n/* __APPLE__ */\n"
         'const char* marker = "__APPLE__";\n::fdatasync(fd);\n', 1, [4]),
        ("masked calls", "// ::fdatasync(fd);\n/* ::fdatasync(fd); */\n"
         'const char* text = "::fdatasync(fd);";\n'
         'const char* raw = R"tag(::fdatasync(fd);)tag";\n', 0, []),
        ("continued condition", "#if !defined(\\\n__APPLE__)\n"
         "::fdatasync(fd);\n#endif\n", 1, []),
        ("continued comment", "// ignored \\\n#ifndef __APPLE__\n"
         "::fdatasync(fd);\n", 1, [3]),
        ("split call", "::\nfdatasync\n(fd);\n", 1, [1]),
        ("digit separator", "auto size = 1'024;\n::fdatasync(fd);\n", 1, [2]),
    )
    for label, text, expected_count, expected in cases:
        count, exposed = _apple_fdatasync(text)
        if (count, exposed) != (expected_count, expected):
            print(f"native source guard: portability self-test failed: {label}: "
                  f"{(count, exposed)!r} != {(expected_count, expected)!r}", file=sys.stderr)
            return 1
    return 0


def self_test() -> int:
    if portability_self_test():
        return 1
    with tempfile.TemporaryDirectory(prefix="pf-native-source-guard-") as temporary:
        fixture = Path(temporary) / "include/pineforge/engine.hpp"
        fixture.parent.mkdir(parents=True)
        fixture.write_text("int " + _PENDING_ROSTER + ";\n", encoding="utf-8")
        diagnostic = io.StringIO()
        with redirect_stderr(diagnostic):
            result = run_scan(FORBIDDEN_IDENTIFIER, [fixture], "source/Pine identifier",
                              exclude_line=excluded_identifier_line)
    output = diagnostic.getvalue()
    if (result != 1
            or "native source guard: forbidden source/Pine identifier found" not in output
            or f"{fixture}:1:int {_PENDING_ROSTER};" not in output):
        print("native source guard: self-test failed", file=sys.stderr)
        return 1

    # The declaration scan must distinguish a genuinely missing member from
    # inline/pure-virtual members, fields with call-style initializers, and
    # declarations carrying default parameters.  This fixture is deliberately
    # independent of the repository's large public header.
    with tempfile.TemporaryDirectory(prefix="pf-native-source-members-") as temporary:
        root = Path(temporary)
        header = root / "include/pineforge/engine.hpp"
        source = root / "src/engine.cpp"
        header.parent.mkdir(parents=True)
        source.parent.mkdir(parents=True)
        header.write_text(
            "class BacktestEngine {\n"
            "public:\n"
            "  void missing_member(int value = 1);\n"
            "  void defined_member();\n"
            "  inline void inline_member() {}\n"
            "  virtual void pure_member() = 0;\n"
            "  int initialized = std::numeric_limits<int>::min();\n"
            "  const char* text = \"void fake_member();\";\n"
            "};\n", encoding="utf-8")
        source.write_text("void BacktestEngine::defined_member() {}\n", encoding="utf-8")
        missing = _declared_but_undefined(root)
        if missing != [(3, "missing_member")]:
            print("native source guard: member-declaration self-test failed: "
                  + repr(missing), file=sys.stderr)
            return 1
        # The unrelated row-source members have no BacktestEngine definitions.
        # A forward declaration before them must still lead to the actual
        # engine definition, including its genuinely missing member.
        header.write_text(
            "class BacktestEngine;\n"
            "class OpenReportRowSource {\n"
            "public:\n"
            "  void clear();\n"
            "  void begin();\n"
            "  int row() const;\n"
            "  void materialize();\n"
            "};\n" + header.read_text(encoding="utf-8"), encoding="utf-8")
        missing = _declared_but_undefined(root)
        if missing != [(11, "missing_member")]:
            print("native source guard: forward-declaration self-test failed: "
                  + repr(missing), file=sys.stderr)
            return 1
    return 0


def main() -> int:
    if self_test():
        return 1
    if run_portability_scan(ROOT):
        return 1
    roots = native_roots()
    if not all(path.is_file() for path in roots):
        missing = [str(path) for path in roots if not path.is_file()]
        print("native source guard: missing roots: " + ", ".join(missing), file=sys.stderr)
        return 1
    if run_scan(FORBIDDEN_INCLUDE, roots, "source/Pine include"):
        return 1
    if run_scan(FORBIDDEN_C_ABI, [ROOT / "src/c_abi.cpp"], "C ABI source name"):
        return 1
    roots = identifier_roots()
    if not all(path.is_file() for path in roots):
        missing = [str(path) for path in roots if not path.is_file()]
        print("native source guard: missing roots: " + ", ".join(missing), file=sys.stderr)
        return 1
    if run_scan(FORBIDDEN_IDENTIFIER, roots, "source/Pine identifier",
                exclude_line=excluded_identifier_line):
        return 1
    undefined = _declared_but_undefined(ROOT)
    if undefined:
        print("native source guard: declared-but-undefined BacktestEngine "
              "member(s) found", file=sys.stderr)
        for line, name in undefined:
            print(f"{ROOT / 'include/pineforge/engine.hpp'}:{line}: {name}",
                  file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
