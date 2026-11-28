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

    def assert_rows(self, source: str):
        self.assertEqual(self.scan_text(source), expected(source))

    def test_bare_callable_binds_at_the_call_site(self):
        self.assert_rows("""
        #include <random>
        std::mt19937 gen{7};
        void take(int, int);
        int pick(int) { return gen(); }
        template<typename F> void apply(F&& pick) { take(pick(1), gen()); }
        void indirect(int (*pick)(int)) { take(pick(1), gen()); }
        void local() {
            auto pick = [](int n) { return n; };
            take(pick(1), gen());
        }
        void first() {
            auto choose = [] { return gen(); };
            take(choose(), gen());  // @ARG
        }
        void second() {
            auto choose = [] { return 3; };
            take(choose(), gen());
        }
        void unshadowed() { take(pick(1), gen()); }  // @ARG
        """)

    def test_arrow_before_braced_argument(self):
        self.assert_rows("""
        #include <random>
        struct Item { unsigned long value; };
        struct Box { Item item; };
        void use(Item, unsigned long);
        void fill(Box* box, std::mt19937& gen) {
            use(box->item = Item{gen()}, gen());  // @ARG
            use(box->item = Item{1}, gen());
        }
        """)

    def test_drawer_visits_statement_blocks_but_owns_its_local_engine(self):
        self.assert_rows("""
        #include <random>
        std::mt19937 shared{7};
        void take(int, int);
        int branch(bool yes) { if (yes) { return shared(); } return 0; }
        int loop(int n) { while (n--) { return branch(true); } return 0; }
        int owned(bool yes) {
            std::mt19937 local{7};
            if (yes) { return local(); }
            return local() + local();  // @OPERAND
        }
        int persistent(bool yes) {
            static std::mt19937 state{3};
            if (yes) { return state(); }
            return 0;
        }
        int borrowed(std::mt19937& source) { return source(); }
        int mixed(std::mt19937& source) { (void)source(); return shared(); }
        int forwarded() { std::mt19937 fresh{3}; return borrowed(fresh); }
        int still_shared() { std::mt19937 fresh{3}; return mixed(fresh); }
        int captured() {
            std::mt19937 fresh{4};
            auto next = [&] { return fresh(); };
            take(next(), next());  // @ARG
            return next();
        }
        int deferred() { if (true) { auto later = [] { return shared(); }; } return 0; }
        void callers() {
            take(branch(true), shared());  // @ARG
            take(loop(1), shared());  // @ARG
            take(persistent(true), persistent(true));  // @ARG
            take(owned(true), shared());
            take(forwarded(), shared());
            take(still_shared(), shared());  // @ARG
            take(captured(), shared());
            take(deferred(), shared());
        }
        """)

    def test_case_call_and_braced_conditional_middle(self):
        source = """
        #include <random>
        constexpr int key(int n) { return n; }
        void act();
        void select(int n) { switch (n) { case key(1): act(); break; } }
        struct Num { int n; };
        int join(int, int);
        int value(bool c, Num* p, std::mt19937& gen, int fallback) {
            return c ? p->n + Num{1}.n + join(gen(), gen()) : fallback;  // @ARG
        }
        """
        toks = lint.tokenize(lint.strip_source(source))
        label = next(i for i, t in enumerate(toks) if t.text == 'case')
        self.assertEqual(lint.Structure(toks).kind[label + 2], lint.CALL)
        self.assert_rows(source)

    def test_generator_spelling_has_lexical_scope(self):
        self.assert_rows("""
        #include <random>
        void take(int, int);
        struct Value { int operator()() const { return 1; } int get() { return 2; } };
        int inspect(Value*);
        void engines(std::mt19937& actual, std::mt19937* ptr) {
            take(actual(), (*ptr)());  // @ARG
        }
        void plain(Value actual, Value* ptr) {
            take(inspect(&actual), inspect(&actual));
            take(actual.get(), actual());
            take((*ptr)(), (*ptr)());
        }
        struct Book {
            std::mt19937 actual{1};
            void use() {
                { Value actual; take(actual(), this->actual()); }
                take(actual(), this->actual());  // @ARG
            }
        };
        """)

    def test_last_mem_initializer_is_walked_as_an_expression(self):
        source = """
        #include <random>
        std::mt19937 gen{1};
        int combine(int, int);
        struct Member { Member(int); Member(std::mt19937); };
        struct Book {
            Member member;
            Book() : member(combine(gen(), gen())) {}  // @ARG
            Book(int) : member(std::mt19937{2}) { (void)gen(); }
        };
        """
        self.assert_rows(source)
        facts = lint.collect_facts(Path('fixture.cpp'), 'fixture.cpp', source)
        self.assertNotIn('member', [fn.name for fn in facts.functions])

    def test_qualified_template_type_does_not_call_free_function(self):
        self.assert_rows("""
        #include <limits>
        #include <random>
        std::mt19937 gen{3};
        void take(int, int);
        int max() { return gen(); }
        template<typename T> struct Counter { static int max() { return gen(); } };
        void compare() {
            take(std::numeric_limits<unsigned>::max(), gen());
            take(Counter<int>::max(), gen());  // @ARG
            take(max(), gen());  // @ARG
        }
        """)

    def test_generator_at_end_of_typed_member_chain(self):
        self.assert_rows(GENERATOR + """
        struct Inner { Random seq{3}; };
        struct Outer { Inner inner; };
        struct Count { int below(int n) { return n; } };
        struct Other { Count seq; };
        void take(int, int);
        void visit(Outer* state, Other& other) {
            take(state->inner.seq.below(3), state->inner.seq.below(4));  // @ARG
            take(other.seq.below(3), state->inner.seq.below(4));
        }
        void callback(void* opaque) {
            auto& state = *static_cast<Outer*>(opaque);
            take(state.inner.seq.below(3), state.inner.seq.below(4));  // @ARG
        }
        """)

    def test_qualified_nested_class_owns_its_methods(self):
        source = """
        struct Outer { struct Inner; };
        struct Outer::Inner { int read() { return 1; } };
        """
        facts = lint.collect_facts(Path('fixture.cpp'), 'fixture.cpp', source)
        self.assertCountEqual([c.name for c in facts.classes], ['Outer', 'Inner'])
        self.assertEqual([(f.name, f.owner) for f in facts.functions], [('read', 'Inner')])

    def test_template_parameter_is_not_the_functions_class(self):
        source = """
        template<class A, class B> int visit(A a, B b) { return a(b); }
        template<class V> struct Box { int read() { return 1; } };
        """
        facts = lint.collect_facts(Path('fixture.cpp'), 'fixture.cpp', source)
        self.assertEqual([c.name for c in facts.classes], ['Box'])
        self.assertEqual({f.name: f.owner for f in facts.functions},
                         {'visit': None, 'read': 'Box'})

    def test_keyword_trailing_return_and_linkage_bodies(self):
        source = """
        #include <random>
        std::mt19937 gen{1};
        extern "C" { int link_name(); }
        template<typename F> void store(F, int);
        void take(int, int);
        auto roll() -> int { return gen(); }
        void run() {
            store([&]() -> bool { return gen() != 0; }, gen());
            auto later = [&]() -> bool { return gen() != 0; };
            take(later(), roll());  // @ARG
        }
        """
        self.assert_rows(source)
        toks = lint.tokenize(lint.strip_source(source))
        linkage = next(i for i, t in enumerate(toks) if t.text == 'extern')
        self.assertEqual(lint.Structure(toks).kind[linkage + 2], lint.BLOCK)

    def test_templated_callee_in_conditional_middle(self):
        self.assert_rows("""
        #include <random>
        template<typename T> int convert(T);
        int read(bool c, std::mt19937& gen, int fallback) {
            return c ? convert<int>(gen() + gen()) : fallback;  // @OPERAND
        }
        """)

    def test_conditional_middle_in_nonfinal_mem_initializer(self):
        self.assert_rows("""
        #include <random>
        int join(int, int);
        struct Pair {
            int first, last;
            Pair(bool c, std::mt19937& gen, int fallback)
                : first(c ? join(gen(), gen()) : fallback), last(0) {}  // @ARG
        };
        """)

    def test_no_draw_cpp_bytes(self):
        cpp = b'// generated C++\r\nint value(int x) { return x + 1; }\r\n'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / 'strategy.cpp'
            path.write_bytes(cpp)
            self.assertEqual(lint.scan(root, [path.name]), [])
            self.assertEqual(path.read_bytes(), cpp)

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

    def test_prototypes_do_not_hide_free_or_member_drawers(self):
        header = """
        #include <cstddef>
        void take(int, int);
        int pick(std::size_t n);
        int pick(std::size_t);
        struct Host { int step(std::size_t) const; };
        """
        unit = """
        #include "api.hpp"
        #include <random>
        std::mt19937 engine{7};
        void visit(Host& h) { take(pick(3), h.step(4)); }  // @ARG
        int pick(std::size_t n) { return engine() % n; }
        int Host::step(std::size_t n) const { return engine() % n; }
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'api.hpp').write_text(textwrap.dedent(header), encoding='utf-8')
            (root / 'unit.cpp').write_text(textwrap.dedent(unit), encoding='utf-8')
            self.assertEqual({(f.line, f.kind) for f in lint.scan(root, ['unit.cpp'])},
                             expected(unit))

    def test_external_receiver_can_dispatch_to_a_drawing_method(self):
        self.assert_rows(GENERATOR + """
        #include <memory>
        struct Host {
            Random engine{7};
            int first() { return engine.below(3); }
            int second() { return engine.below(4); }
        };
        void take(int, int);
        void visit(std::unique_ptr<Host>& h) { take(h->first(), h->second()); }  // @ARG
        """)

    def test_explicit_callable_type_keeps_its_lambda_draw(self):
        self.assert_rows("""
        #include <functional>
        #include <random>
        void take(int, int);
        void visit(std::mt19937& engine) {
            std::function<unsigned long()> next = [&engine] { return engine(); };
            take(next(), next());  // @ARG
        }
        """)

    def test_anonymous_namespace_engine_visible_in_enclosing_namespace(self):
        self.assert_rows("""
        #include <random>
        void take(int, int);
        namespace { std::mt19937 file_engine{7}; }
        void visit() { take(file_engine(), file_engine()); }  // @ARG
        namespace session {
            namespace { std::mt19937 session_engine{9}; }
            void visit() { take(session_engine(), session_engine()); }  // @ARG
        }
        """)

    def test_generator_through_template_parameter_and_reference_alias(self):
        self.assert_rows("""
        #include <random>
        void take(int, int);
        template<typename G> void roll(G& rng) { take(rng(), rng()); }  // @ARG
        void run() { std::mt19937 rng(7); roll(rng); }
        void aliased(std::mt19937& source) {
            auto& rng = source;
            take(rng(), rng());  // @ARG
        }
        """)

    def test_parenthesized_product_is_an_initializer(self):
        self.assert_rows(GENERATOR + """
        void take(int, int);
        int a = 2;
        int b = 3;
        Random rng(a * b);
        void visit() { take(rng(), rng()); }  // @ARG
        """)

    def test_variadic_and_function_pointer_prototypes_remain_prototypes(self):
        self.assert_rows("""
        #include <random>
        void take(int, int);
        std::mt19937 engine{7};
        int variadic(int, ...);
        int callback(int (*cb)(int));
        int variadic(int, ...) { return engine(); }
        int callback(int (*cb)(int)) { return engine(); }
        void visit() {
            take(variadic(1, 2), engine());  // @ARG
            take(callback(nullptr), engine());  // @ARG
        }
        """)


if __name__ == '__main__':
    unittest.main()
