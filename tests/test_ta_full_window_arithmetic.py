"""Exercise the bounds and tie midpoint used by the indicator loops."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile


SRC = Path(__file__).resolve().parents[1] / "src"


def one(pattern, text):
    matches = re.findall(pattern, text, re.DOTALL)
    if len(matches) != 1:
        raise ValueError(f"expected one match for {pattern!r}, found {len(matches)}")
    return matches[0]


def body(source, method):
    source = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.DOTALL)
    return one(r"(?m)^double " + re.escape(method) + r"\([^)]*\) \{(.*?)^\}", source)


def traversal(method, first, last):
    # Sample the bounds of the live loop, including the types of its indices.
    init, cond, step = one(r"\bfor \(([^;]+);([^;]+);([^{};]+?)\) \{", method)
    start = (one(r"\n\s*([\w: ]+\bstart\s*=[^;]+;)", method)
             if re.search(r"\bstart\b", init) else "")
    return f"""
            constexpr Window buffer_{{static_cast<std::size_t>(length_) + 1}};
            {start}
            {init};
            if (i != {first} || !({cond})) return false;
            i = {last};
            if (!({cond})) return false;
            {step};
            return !({cond}) && i == static_cast<std::size_t>({last}) + 1;"""


def probe():
    oscillators = (SRC / "ta_oscillators.cpp").read_text()
    misc = (SRC / "ta_misc.cpp").read_text()
    cases = {row: traversal(body(source, row), first, last) for row, source, first, last in (
        ("Rising::compute", oscillators, 1, "INT_MAX"),
        ("Falling::compute", oscillators, 1, "INT_MAX"),
        ("PercentRank::compute", misc, 0, "INT_MAX - 1"),
        ("PercentRank::recompute", misc, 0, "INT_MAX - 1"),
    )}
    rci = body(oscillators, "RCI::calculate")
    i_type = one(r"\n    ([\w:]+) i = 0;", rci)
    j_type = one(r"\n        ([\w:]+) j = i;", rci)
    midpoint = one(r"\bdouble avg_rank = ([^;]+);", rci)
    for name, offset in (("singleton", 0), ("tie", 1)):
        cases[f"RCI {name} midpoint"] = f"""
            {i_type} i = length_ / 2 + 1;
            {j_type} j = i + {offset};
            return ({midpoint}) == static_cast<double>(i) + {offset} * 0.5;"""

    # A signed overflow disqualifies a constant expression. Overload
    # resolution turns that particular observation into a failed row without
    # executing undefined arithmetic or preventing the other rows from running.
    program = """
#include <climits>
#include <cstddef>
#include <cstdio>
struct Window { std::size_t n; constexpr std::size_t size() const { return n; } };
template<auto F, bool ok = F()> constexpr bool observed(int) { return ok; }
template<auto F> constexpr bool observed(...) { return false; }
static_assert(INT_MAX == 2147483647, "the boundary cases require 32-bit int");
"""
    rows = []
    for number, (name, code) in enumerate(cases.items()):
        program += f"template<int length_> constexpr bool row{number}() {{{code}\n}}\n"
        rows.append(f'    check("{name}", observed<row{number}<INT_MAX>>(0));')
    return program + """
int main() {
    int failures = 0;
    auto check = [&](const char* name, bool ok) {
        std::printf("%s %s\\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
""" + "\n".join(rows) + "\n    return failures != 0;\n}\n"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        executable = Path(directory) / "window-bounds"
        try:
            subprocess.run([sys.argv[1], "-std=c++17", "-x", "c++", "-", "-o", executable],
                           input=probe(), text=True, check=True)
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            sys.exit(f"NOT COMPARED: could not build the current arithmetic: {error}")
        sys.exit(subprocess.run([executable]).returncode)
