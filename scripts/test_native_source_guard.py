#!/usr/bin/env python3
"""Fail closed if a native/generic include root reaches a source/Pine header.

This is the inexpensive textual companion to check_native_include_independence:
the latter proves the installed closure and object symbols after a build; this
guard runs in preflight and rejects a forbidden include before compilation.
"""
from __future__ import annotations

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
    match = re.search(r"\bclass\s+BacktestEngine\b[^\{]*\{", masked)
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


def self_test() -> int:
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
    return 0


def main() -> int:
    if self_test():
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
