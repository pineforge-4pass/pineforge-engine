#!/usr/bin/env python3
"""Self-test for check_rng_draw_order.py.

The must-fail fixture holds XPLAT1's three shapes (e8bc3237's parent) and the
shapes the tenth-site audit found (a drawing helper in one argument, a nested
call's arguments, a distribution, an operator's two operands, a lambda over a
captured seed, a generator taken by value).  The must-pass fixture holds every
sequenced form the checker must leave alone, and the one-draw-per-statement
rewrite of each must-fail line.  Run: python3 scripts/test_check_rng_draw_order.py
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_rng_draw_order as lint  # noqa: E402

GENERATOR = """
#include <cstdint>
#include <random>
#include <sstream>
#include <utility>
#include <vector>
struct Random {
    std::uint64_t state;
    explicit Random(std::uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    bool chance(int p) { return below(100) < p; }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};
"""

# Each line that must be reported ends with a marker comment naming the kind.
MUST_FAIL = GENERATOR + """
void strategy_exit(const char*, const char*, double, double, double, double);
void strategy_entry(const char*, bool, double, const char*, const char*, int);
void strategy_order(const char*, bool, double, double);
static const char* const kExits[] = {"X", "Y"};
static const double kNa = 0.0;

void xplat1(Random& s, const char* id, bool is_long, double close) {
    // e8bc3237^ case 7, case 4 and default
    strategy_exit(kExits[s.below(2)], id, kNa, kNa, 4.0 + s.below(8),  // @ARG
                  1.0 + s.below(4));
    strategy_entry(id, is_long, 1.0 + s.below(3), "", s.chance(30) ? "g" : "",  // @ARG
                   s.chance(30) ? 1 + s.below(2) : 0);
    strategy_order(id, is_long, 1.0 + s.below(2),  // @ARG
                   s.chance(50) ? (is_long ? close - 1 : close + 1) : kNa);
}

struct Book {
    Random rng_{7};
    const char* long_id() { return rng_.chance(70) ? "L" : "L2"; }
    double units() {
        static const double sizes[] = {1.0, 2.0, 0.5};
        return sizes[rng_.below(3)];
    }
    int opening(bool buy, double size, long ref) {
        const int base = buy ? int(size) : -int(size);
        return rng_.chance(30) ? base + int(ref) : base;
    }
    void place(int request, bool market);
    void close(const char* id, double qty, bool immediately);
    void enter(long ref) {
        place(opening(true, 1.0, ref), rng_.chance(30));  // @ARG
        const int replaced = opening(rng_.chance(50), units(), ref);  // @ARG
        close(long_id(), 1.0, rng_.chance(30));  // @ARG
        (void)replaced;
    }
    long level(long ref) { return ref + (rng_.chance(50) ? 1 : -1) * rng_.below(16); }  // @OPERAND
};

struct Kc { double compute(double src, double high, double low, double close); };
void noise(Kc& kc, Random& r) {
    kc.compute(1500.0 + r.unit() * 10.0, 1510.0, 1490.0 + r.unit(), 1500.0);  // @ARG
}

void engines() {
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<int> dist(0, 9);
    const auto pair = std::make_pair(dist(rng), dist(rng));  // @ARG
    const bool same = rng() % 2 == rng() % 3;  // @OPERAND
    std::uint64_t state = 1;
    auto uniform = [&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    const double sum = static_cast<double>(uniform()) + static_cast<double>(uniform());  // @OPERAND
    auto later = [&rng] { return rng() - rng(); };  // @OPERAND
    (void)pair; (void)same; (void)sum; (void)later;
}

std::uint64_t draw_twice(Random copy) {  // @BYVALUE
    return copy.next();
}
"""

MUST_PASS = GENERATOR + """
struct Request { int a; int b; };
struct Fold {
    std::uint64_t h = 1;
    void u(std::uint64_t v) {
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
};
std::uint64_t value(const Fold& f) { return f.h; }
std::vector<int> walk(Random& r, int count, double wide);
void take(int a, int b);
void strategy_exit(const char*, const char*, double, double, double, double);
static const char* const kExits[] = {"X", "Y"};

struct Tracker { std::uint64_t step(int v) { return static_cast<std::uint64_t>(v) * 2u; } };
struct Twin {
    Random rng_{3};
    Tracker staged_tracker_;
    Tracker direct_tracker_;
    void step(int n) { for (int k = 0; k < n; ++k) (void)rng_.next(); }
    bool agree() { return staged_tracker_.step(1) == direct_tracker_.step(2); }
};

void sequenced(Random& r, Random& s, const char* id) {
    const int a = r.below(3), b = r.below(4);
    Request q{r.chance(50) ? 1 : 2, r.chance(30) ? 3 : 4};
    Request w = {r.below(3), r.below(4)};
    std::vector<Request> rows;
    rows.push_back({r.below(3), r.below(4)});
    int x = r.chance(50) ? r.below(3) : r.below(4);
    const bool y = r.chance(50) && r.chance(50);
    const bool z = r.chance(50) || r.chance(50);
    int c = (r.below(3), r.below(4));
    std::ostringstream os;
    os << r.below(3) << r.below(4);
    int slots[16] = {};
    slots[r.below(8)] = r.below(9);
    if (r.chance(7)) x += r.chance(50) ? r.below(12) : -r.below(12);
    const std::vector<int> bars = walk(r, 40 + r.below(20), 10.0);
    const int t1 = int(r.unit() * 6.0), t2 = static_cast<int>(r.unit() * 6.0);
    const int nested = r.below(r.below(3) + 1);
    std::vector<std::vector<int>> grid;
    // take(r.below(3), r.below(4)) in a comment is not code
    const char* text = "take(r.below(3), r.below(4))";
    // the one-draw-per-statement rewrite of XPLAT1's case 7
    const char* const exit_id = kExits[s.below(2)];
    const double trail_points = 4.0 + s.below(8);
    const double trail_offset = 1.0 + s.below(4);
    strategy_exit(exit_id, id, 0.0, 0.0, trail_points, trail_offset);
    const int first = r.below(3);
    const int second = r.below(4);
    take(first, second);
    Fold f, g;
    const bool folds_agree = value(f) == value(g);
    (void)a; (void)b; (void)q; (void)w; (void)y; (void)z; (void)c; (void)bars; (void)t1;
    (void)t2; (void)nested; (void)text; (void)folds_agree; (void)grid;
}
"""


def expected(text: str) -> set[tuple[int, str]]:
    out = set()
    for n, line in enumerate(text.splitlines(), 1):
        if '// @' in line:
            out.add((n, line.rsplit('// @', 1)[1].strip()))
    return out


class DrawOrderTest(unittest.TestCase):
    def scan_text(self, text: str, name: str = 'fixture.cpp') -> set[tuple[int, str]]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'tests').mkdir()
            (root / 'tests' / name).write_text(textwrap.dedent(text), encoding='utf-8')
            return {(f.line, f.kind) for f in lint.scan(root, ['tests'])}

    def test_must_fail_reports_every_marked_line_and_nothing_else(self):
        self.assertEqual(self.scan_text(MUST_FAIL), expected(MUST_FAIL))

    def test_must_pass_is_clean(self):
        self.assertEqual(self.scan_text(MUST_PASS), set())

    def test_sequencing_a_site_clears_it(self):
        broken = GENERATOR + """
        void take(int a, int b);
        void f(Random& r) { take(r.below(3), r.below(4)); }
        """
        fixed = GENERATOR + """
        void take(int a, int b);
        void f(Random& r) {
            const int a = r.below(3);
            const int b = r.below(4);
            take(a, b);
        }
        """
        self.assertEqual({k for _, k in self.scan_text(broken)}, {'ARG'})
        self.assertEqual(self.scan_text(fixed), set())

    def test_a_helper_header_is_checked_under_the_unit_that_includes_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'tests').mkdir()
            (root / 'tests' / 'gen.hpp').write_text(GENERATOR, encoding='utf-8')
            (root / 'tests' / 'user.cpp').write_text(
                '#include "gen.hpp"\nvoid take(int, int);\n'
                'void f(Random& r) { take(r.below(3), r.below(4)); }\n', encoding='utf-8')
            found = {(f.rel, f.line, f.kind) for f in lint.scan(root, ['tests'])}
        self.assertEqual(found, {('tests/user.cpp', 3, 'ARG')})

    def test_cli_exit_codes(self):
        for text, code in ((MUST_FAIL, 1), (MUST_PASS, 0)):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / 'tests').mkdir()
                (root / 'tests' / 'fixture.cpp').write_text(text, encoding='utf-8')
                proc = subprocess.run(
                    [sys.executable, str(HERE / 'check_rng_draw_order.py'), '--root', str(root),
                     'tests'], capture_output=True, text=True)
                self.assertEqual(proc.returncode, code, proc.stdout + proc.stderr)


if __name__ == '__main__':
    unittest.main()
