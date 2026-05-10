#include <pineforge/generic_matrix.hpp>
#include <cassert>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

using pineforge::PineGenericMatrix;

struct Pivot {
    int timestamp_idx{0};
    double price{0.0};
    std::string label{};
    std::vector<double> history{};
};

static void test_udt_new_get_set() {
    auto m = PineGenericMatrix<Pivot>::new_(2, 2);  // T{} default
    assert(m.rows() == 2);
    assert(m.get(0, 0).timestamp_idx == 0);
    assert(m.get(0, 0).price == 0.0);
    assert(m.get(0, 0).label.empty());

    Pivot p{42, 100.5, "high", {1.0, 2.0, 3.0}};
    m.set(0, 0, p);
    auto got = m.get(0, 0);
    assert(got.timestamp_idx == 42);
    assert(got.price == 100.5);
    assert(got.label == "high");
    assert(got.history.size() == 3 && got.history[2] == 3.0);
}

static void test_udt_deep_copy() {
    auto m = PineGenericMatrix<Pivot>::new_(1, 1);
    Pivot p{1, 2.0, "x", {9.0}};
    m.set(0, 0, p);
    auto sub = m.submatrix(0, 1, 0, 1);
    sub.set(0, 0, Pivot{99, 88.0, "y", {7.0}});
    auto orig = m.get(0, 0);
    assert(orig.timestamp_idx == 1);  // submatrix is deep copy
    assert(orig.label == "x");
}

static void test_udt_add_row_and_row() {
    auto m = PineGenericMatrix<Pivot>::new_(0, 0);
    Pivot p{5, 10.0, "z", {}};
    m.add_row(0, std::vector<Pivot>{p});
    assert(m.rows() == 1 && m.columns() == 1);
    auto row = m.row(0);
    assert(row.size() == 1 && row[0].label == "z");
}

static void test_udt_default_constructible_transpose_reshape() {
    auto m = PineGenericMatrix<Pivot>::new_(2, 3);
    m.set(0, 0, Pivot{1, 1.0, "a", {}});
    m.set(1, 2, Pivot{2, 2.0, "b", {}});
    auto t = m.transpose();
    assert(t.rows() == 3 && t.columns() == 2);
    assert(t.get(0, 0).label == "a");
    assert(t.get(2, 1).label == "b");
    m.reshape(3, 2);
    assert(m.rows() == 3 && m.columns() == 2);
}

// SFINAE probe — a UDT without default ctor must not satisfy the
// requirements of transpose()/reshape(). We verify static_assert wiring
// indirectly by confirming std::is_default_constructible_v reports false
// for the type, which is what the static_assert in the header keys off.
struct NoDefault {
    int x;
    NoDefault() = delete;
    explicit NoDefault(int v) : x(v) {}
};

static void test_udt_no_default_ctor_compile_guard() {
    static_assert(!std::is_default_constructible_v<NoDefault>,
                  "NoDefault must not be default-constructible for this guard test");
    // The matrix type itself can still be instantiated; only transpose()/reshape()
    // would fail to compile (verified by static_assert in the header).
    PineGenericMatrix<NoDefault> m;
    (void)m;
}

// --- M4: strong exception guarantee ---
// Throwing-copy UDT: nth copy ctor throws.
struct ThrowOnCopy {
    int v{0};
    static int copies;
    static int throw_at;  // 0 disables
    ThrowOnCopy() = default;
    explicit ThrowOnCopy(int x) : v(x) {}
    ThrowOnCopy(const ThrowOnCopy& o) : v(o.v) {
        ++copies;
        if (throw_at != 0 && copies == throw_at) throw std::runtime_error("boom");
    }
    ThrowOnCopy& operator=(const ThrowOnCopy&) = default;
    ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
    ThrowOnCopy& operator=(ThrowOnCopy&&) noexcept = default;
};
int ThrowOnCopy::copies = 0;
int ThrowOnCopy::throw_at = 0;

static void prime_matrix(PineGenericMatrix<ThrowOnCopy>& m) {
    int v = 0;
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.columns(); ++c)
            m.set(r, c, ThrowOnCopy{++v});
}

static void test_strong_guarantee_add_col() {
    auto m = PineGenericMatrix<ThrowOnCopy>::new_(3, 2);
    prime_matrix(m);
    int snapshot_v00 = m.get(0, 0).v;
    int snapshot_v21 = m.get(2, 1).v;
    int orig_cols = m.columns();
    ThrowOnCopy::copies = 0;
    ThrowOnCopy::throw_at = 5;  // mid-loop
    bool threw = false;
    try {
        std::vector<ThrowOnCopy> col{ThrowOnCopy{100}, ThrowOnCopy{101}, ThrowOnCopy{102}};
        m.add_col(0, col);
    } catch (const std::runtime_error&) { threw = true; }
    ThrowOnCopy::throw_at = 0;
    assert(threw);
    assert(m.columns() == orig_cols);  // unchanged
    assert(m.get(0, 0).v == snapshot_v00);
    assert(m.get(2, 1).v == snapshot_v21);
}

static void test_strong_guarantee_remove_col() {
    auto m = PineGenericMatrix<ThrowOnCopy>::new_(3, 3);
    prime_matrix(m);
    int orig_cols = m.columns();
    int snapshot_v00 = m.get(0, 0).v;
    int snapshot_v22 = m.get(2, 2).v;
    ThrowOnCopy::copies = 0;
    ThrowOnCopy::throw_at = 4;
    bool threw = false;
    try { m.remove_col(1); } catch (const std::runtime_error&) { threw = true; }
    ThrowOnCopy::throw_at = 0;
    assert(threw);
    assert(m.columns() == orig_cols);
    assert(m.get(0, 0).v == snapshot_v00);
    assert(m.get(2, 2).v == snapshot_v22);
}

int main() {
    test_strong_guarantee_add_col();
    test_strong_guarantee_remove_col();
    test_udt_new_get_set();
    test_udt_deep_copy();
    test_udt_add_row_and_row();
    test_udt_default_constructible_transpose_reshape();
    test_udt_no_default_ctor_compile_guard();
    std::printf("All test_generic_matrix_udt tests passed.\n");
    return 0;
}
