#!/usr/bin/env python3
"""Source guard for the CommandScratchStore / ScratchReadback / ScratchBacking headers.

The behavioural fixtures (test_scratch_backing, test_scratch_store)
prove what the contract does; this guard pins what the headers must never
contain, so a later lane cannot weaken the contract by editing them:

* ScratchStatus is a closed vocabulary. There is no capacity, refusal or
  exhaustion member: backing I/O faults are io_error; caller contract
  violations are programming_error.
* No exception machinery, no growing container, no heap call and no
  RunFailureCode conversion appears in code (comments and strings are removed
  before scanning).
* The three contract headers allocate nothing at all. The definition header
  scratch_store_impl.hpp allocates in exactly one place, the fixed workspace of
  the constructor, and releases it in exactly one place, the destructor: one
  operator new and one placement new, one operator delete.

The detectors are exercised on built-in positive and negative samples before
the headers are read, so a regex that stopped matching fails here instead of
silently passing the real files.

Predicted first red on 6cfcff0696: the headers do not exist there, so every
read below fails. No execution is claimed by the lane that wrote this.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DETAIL = ROOT / "include" / "pineforge" / "detail"
CONTRACT = ("scratch_backing.hpp", "scratch_readback.hpp", "scratch_store.hpp")
DEFINITION = ("scratch_store_impl.hpp",)
STATUS = ["ok", "end", "cancelled", "not_found", "programming_error", "io_error", "corrupt"]
WORKSPACE_NEW_MAX = 2
WORKSPACE_DELETE = 1

LITERAL_OR_COMMENT = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.DOTALL)
DELETED = re.compile(r"=\s*delete\b")
NEW_TOKEN = re.compile(r"(?<!<)\bnew\b(?!>)")
DELETE_TOKEN = re.compile(r"\bdelete\b")
FORBIDDEN_CODE = (
    ("throw", re.compile(r"\bthrow\b")),
    ("try", re.compile(r"\btry\b")),
    ("catch", re.compile(r"\bcatch\b")),
    ("bad_alloc", re.compile(r"\bbad_alloc\b")),
    ("growing container or owner",
     re.compile(r"\bstd::(?:vector|map|multimap|set|multiset|unordered_\w+|deque|list|"
                r"forward_list|string|function|shared_ptr|unique_ptr)\b")),
    ("heap call", re.compile(r"\b(?:malloc|calloc|realloc|free)\s*\(")),
    ("run failure conversion", re.compile(r"\bRunFail\w*|\brun_failure\w*")),
)
FORBIDDEN_INCLUDE = re.compile(
    r"^\s*#\s*include\s*<(?:vector|map|set|string|unordered_map|unordered_set|deque|list|"
    r"functional|memory|exception|stdexcept|system_error)>", re.MULTILINE)
STATUS_ENUM = re.compile(r"enum\s+class\s+ScratchStatus\s*:\s*std::uint32_t\s*\{(.*?)\}", re.DOTALL)


def strip(text: str) -> str:
    """Drop comments and literals, keeping line structure."""
    def blank(match: re.Match) -> str:
        return "\n" * match.group(0).count("\n")
    return DELETED.sub("", LITERAL_OR_COMMENT.sub(blank, text))


def violations(text: str, workspace: bool = False) -> list[str]:
    """Findings in one header's code. `workspace` is the definition header,
    which owns the single bounded allocation; everything else owns none."""
    code = strip(text)
    found = [label for label, pattern in FORBIDDEN_CODE if pattern.search(code)]
    if FORBIDDEN_INCLUDE.search(code):
        found.append("forbidden standard header")
    news = len(NEW_TOKEN.findall(code))
    deletes = len(DELETE_TOKEN.findall(code))
    if workspace:
        if news > WORKSPACE_NEW_MAX:
            found.append("new expression")
        if deletes > WORKSPACE_DELETE:
            found.append("delete expression")
    else:
        if news:
            found.append("new expression")
        if deletes:
            found.append("delete expression")
    return found


def status_names(text: str) -> list[str] | None:
    match = STATUS_ENUM.search(strip(text))
    if match is None:
        return None
    names = []
    for item in match.group(1).split(","):
        item = item.strip()
        if item:
            names.append(re.split(r"\s*=\s*", item)[0].strip())
    return names


def self_test() -> list[str]:
    failures: list[str] = []

    def expect(condition: bool, why: str) -> None:
        if not condition:
            failures.append("self-test: " + why)

    expect("throw" in violations("void f() { throw 1; }"), "throw is detected")
    expect("catch" in violations("void f() { try { } catch (...) { } }"), "catch is detected")
    expect("growing container or owner" in violations("std::vector<int> v;"), "vector is detected")
    expect("new expression" in violations("int* p = new int;"), "new is detected")
    expect("delete expression" in violations("void f(int* p) { delete p; }"), "delete is detected")
    expect("heap call" in violations("void f() { void* p = malloc(4); }"), "malloc is detected")
    expect("forbidden standard header" in violations("#include <vector>\n"), "include is detected")
    expect("run failure conversion" in violations("RunFailureCode code;"), "conversion is detected")
    expect(violations("// throw new std::vector\nint a;") == [], "comments are ignored")
    expect(violations('const char* s = "throw new";') == [], "string literals are ignored")
    expect(violations("struct S { S(const S&) = delete; };") == [], "deleted members are allowed")
    expect(violations("void* b = ::operator new(8, std::nothrow); S* s = new (b) S;\n"
                      "void g(void* p) { ::operator delete(p); }", workspace=True) == [],
           "the one bounded workspace allocation is allowed in the definition header")
    expect(violations("#include <new>\nvoid* b = ::operator new(8, std::nothrow); S* s = new (b) S;\n"
                      "void g(void* p) { ::operator delete(p); }", workspace=True) == [],
           "the include of the allocation header is not an allocation")
    expect("new expression" in violations("void* a = ::operator new(8); S* s = new (a) S; T* t = new T;",
                                          workspace=True), "a third allocation site is detected")
    expect("delete expression" in violations("void g(void* p) { ::operator delete(p); delete q; }",
                                             workspace=True), "a second release site is detected")
    expect("new expression" in violations("void* b = ::operator new(8);"),
           "a contract header allocates nothing")
    expect("forbidden standard header" in violations("#include <memory>\n", workspace=True),
           "the definition header includes no owning container or smart pointer")
    expect(status_names("enum class ScratchStatus : std::uint32_t { ok = 0, end, io_error, };")
           == ["ok", "end", "io_error"], "enumerators are read in order")
    expect(status_names("// enum class ScratchStatus : std::uint32_t { ok };") is None,
           "a commented enum is not read")
    return failures


def main() -> int:
    failures = self_test()
    seen_status = False
    for names, workspace in ((CONTRACT, False), (DEFINITION, True)):
        for name in names:
            path = DETAIL / name
            try:
                text = path.read_text(encoding="utf-8")
            except OSError as error:
                failures.append(f"{name}: cannot read: {error}")
                continue
            for label in violations(text, workspace):
                failures.append(f"{name}: contains a {label} in code")
            status = status_names(text)
            if status is not None:
                seen_status = True
                if status != STATUS:
                    failures.append(f"{name}: ScratchStatus is {status}, expected exactly {STATUS}")
            for include in re.findall(r'^\s*#\s*include\s*<(pineforge/[^>]+)>', text, re.MULTILINE):
                if not (ROOT / "include" / include).is_file():
                    failures.append(f"{name}: includes missing header {include}")
    if not seen_status:
        failures.append("no contract header declares ScratchStatus")
    for failure in failures:
        print(failure, file=sys.stderr)
    print(f"scratch contract guard: {len(failures)} findings")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
