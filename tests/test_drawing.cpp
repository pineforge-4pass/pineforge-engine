// Unit tests for the drawing-objects-as-data runtime (include/pineforge/drawing.hpp).
// Spec: docs/drawing-objects-as-data.md §3. Headless data structures — no rendering.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>
#include <pineforge/drawing.hpp>

using namespace pineforge;

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

// Asserts that evaluating `expr` throws pine_drawing_error.
#define CHECK_THROWS(expr)                                                     \
    do {                                                                       \
        bool threw = false;                                                    \
        try { (void)(expr); }                                                  \
        catch (const pine_drawing_error&) { threw = true; }                    \
        catch (...) {}                                                         \
        if (!threw) {                                                          \
            std::printf("FAIL %s:%d: expected pine_drawing_error: %s\n",       \
                        __FILE__, __LINE__, #expr);                            \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

// Asserts that evaluating `stmt` does NOT throw.
#define CHECK_NOTHROW(stmt)                                                    \
    do {                                                                       \
        try { stmt; }                                                          \
        catch (...) {                                                          \
            std::printf("FAIL %s:%d: unexpected throw: %s\n",                  \
                        __FILE__, __LINE__, #stmt);                            \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static constexpr double TOL = 1e-9;
static bool near(double a, double b) { return std::fabs(a - b) <= TOL; }

// ---------------------------------------------------------------------------
// is_na for default handles + na ChartPoint (spec §3.2)
static void test_is_na_defaults() {
    std::printf("test_is_na_defaults\n");
    CHECK(is_na(Line{}));
    CHECK(is_na(Box{}));
    CHECK(is_na(Label{}));
    CHECK(is_na(Linefill{}));

    // na<T>() falls out of na.hpp's generic for the handle types (default {-1}).
    CHECK(is_na(na<Line>()));
    CHECK(is_na(na<Box>()));
    CHECK(is_na(na<Label>()));
    CHECK(is_na(na<Linefill>()));
    CHECK(na<Line>().id == -1);

    // Fully-na ChartPoint is na; a from_index/from_time point is NOT na overall.
    CHECK(is_na(ChartPoint{}));
    ChartPoint from_index{ /*index*/ 7, na<int64_t>(), na<double>() };
    CHECK(!is_na(from_index));
    ChartPoint from_time{ na<int64_t>(), /*time*/ 1000LL, na<double>() };
    CHECK(!is_na(from_time));
    ChartPoint only_price{ na<int64_t>(), na<int64_t>(), 42.0 };
    CHECK(!is_na(only_price));
}

// ---------------------------------------------------------------------------
// arena alloc returns monotonic ids (spec §3.4)
static void test_monotonic_ids() {
    std::printf("test_monotonic_ids\n");
    DrawingArena<LineRec> a;  // default cap 50
    Line l0 = pf_line_new(a, 0, 0.0, 1, 1.0);
    Line l1 = pf_line_new(a, 0, 0.0, 1, 1.0);
    Line l2 = pf_line_new(a, 0, 0.0, 1, 1.0);
    CHECK(l0.id == 0);
    CHECK(l1.id == 1);
    CHECK(l2.id == 2);
    CHECK(!is_na(l0));

    // Deleting then allocating does NOT reuse the id (tombstoned, monotonic).
    pf_line_delete(a, l1);
    Line l3 = pf_line_new(a, 0, 0.0, 1, 1.0);
    CHECK(l3.id == 3);
}

// ---------------------------------------------------------------------------
// *_new(na,na,na,na) allocates a LIVE record with na coords (spec §3.7)
static void test_new_na_coords_is_live() {
    std::printf("test_new_na_coords_is_live\n");
    DrawingArena<LineRec> a;
    Line h = pf_line_new(a, na<int64_t>(), na<double>(), na<int64_t>(), na<double>());
    CHECK(!is_na(h));               // the handle is LIVE
    CHECK(h.id == 0);
    CHECK(is_na(pf_line_get_x1(a, h)));   // ... but its coords are na
    CHECK(is_na(pf_line_get_y1(a, h)));
}

// ---------------------------------------------------------------------------
// mutate-through-handle, incl. via a COPIED handle id (reference semantics)
static void test_mutate_through_handle() {
    std::printf("test_mutate_through_handle\n");
    DrawingArena<LineRec> a;
    Line h = pf_line_new(a, 1, 10.0, 2, 20.0);

    pf_line_set_x1(a, h, 100);
    pf_line_set_y1(a, h, 11.0);
    pf_line_set_xy2(a, h, 200, 22.0);
    CHECK(pf_line_get_x1(a, h) == 100);
    CHECK(near(pf_line_get_y1(a, h), 11.0));
    CHECK(pf_line_get_x2(a, h) == 200);
    CHECK(near(pf_line_get_y2(a, h), 22.0));

    // A by-value copy of the HANDLE (not pf_line_copy) shares the same arena id:
    // mutations through one are visible through the other (reference semantics).
    Line alias = h;
    pf_line_set_x1(a, alias, 999);
    CHECK(pf_line_get_x1(a, h) == 999);

    // chart.point setters select x per the record's stored xloc (bar_index here).
    pf_line_set_first_point(a, h, ChartPoint{ /*index*/ 5, /*time*/ 123456789LL, /*price*/ 50.0 });
    CHECK(pf_line_get_x1(a, h) == 5);          // bar_index -> .index, not .time
    CHECK(near(pf_line_get_y1(a, h), 50.0));
}

// ---------------------------------------------------------------------------
// set_xloc reinterprets (stores new numbers + flag), no coord conversion
static void test_set_xloc_reinterpret() {
    std::printf("test_set_xloc_reinterpret\n");
    DrawingArena<LineRec> a;
    Line h = pf_line_new(a, 1, 10.0, 2, 20.0, XLoc::bar_index);
    int64_t ms1 = 1700000000000LL, ms2 = 1700000600000LL;
    pf_line_set_xloc(a, h, ms1, ms2, XLoc::bar_time);
    CHECK(pf_line_get_x1(a, h) == ms1);   // raw stored value read back, xloc-agnostic
    CHECK(pf_line_get_x2(a, h) == ms2);
    // get_price on a bar_time line errors like TV.
    CHECK_THROWS(pf_line_get_price(a, h, ms1));
}

// ---------------------------------------------------------------------------
// copy() is a deep, independent arena record (spec §3.6 note 7)
static void test_copy_is_deep() {
    std::printf("test_copy_is_deep\n");
    DrawingArena<LineRec> a;
    Line orig = pf_line_new(a, 1, 10.0, 2, 20.0);
    Line cp = pf_line_copy(a, orig);
    CHECK(!is_na(cp));
    CHECK(cp.id != orig.id);                 // distinct arena record

    pf_line_set_y2(a, cp, 999.0);            // mutate the copy
    CHECK(near(pf_line_get_y2(a, orig), 20.0)); // original unchanged
    CHECK(near(pf_line_get_y2(a, cp), 999.0));

    pf_line_set_y1(a, orig, 7.0);            // mutate the original
    CHECK(near(pf_line_get_y1(a, cp), 10.0));   // copy unchanged

    // copy of a dead/na handle -> na handle (silent), not a throw.
    Line na_cp;
    CHECK_NOTHROW(na_cp = pf_line_copy(a, Line{}));
    CHECK(is_na(na_cp));
}

// ---------------------------------------------------------------------------
// delete() then a getter THROWS; double-delete / na-delete are silent (spec §3.7)
static void test_delete_then_getter_throws() {
    std::printf("test_delete_then_getter_throws\n");
    DrawingArena<LineRec> a;
    Line h = pf_line_new(a, 1, 10.0, 2, 20.0);
    pf_line_delete(a, h);

    CHECK_THROWS(pf_line_get_x1(a, h));
    CHECK_THROWS(pf_line_get_y2(a, h));
    CHECK_THROWS(pf_line_set_x1(a, h, 5));        // setter on dead also throws
    CHECK_THROWS(pf_line_get_price(a, h, 3));     // get_price on dead throws

    // getter on a never-allocated na handle throws too.
    CHECK_THROWS(pf_line_get_x1(a, Line{}));

    // double-delete + na-delete are silent no-ops.
    CHECK_NOTHROW(pf_line_delete(a, h));          // already deleted
    CHECK_NOTHROW(pf_line_delete(a, Line{}));     // na handle
    CHECK_NOTHROW(pf_line_delete(a, Line{ 9999 })); // out-of-range id
}

// ---------------------------------------------------------------------------
// FIFO eviction at exact cap (spec §3.8)
static void test_fifo_eviction() {
    std::printf("test_fifo_eviction\n");
    const int cap = 3;
    DrawingArena<LineRec> a(cap);
    Line h[5];
    for (int i = 0; i < 5; ++i)               // create cap+2 = 5
        h[i] = pf_line_new(a, i, (double)i, i + 1, (double)(i + 1));

    // Oldest two (creation order) are tombstoned; getters on them throw.
    CHECK_THROWS(pf_line_get_x1(a, h[0]));
    CHECK_THROWS(pf_line_get_x1(a, h[1]));

    // The newest `cap` survive and read back correctly.
    CHECK(pf_line_get_x1(a, h[2]) == 2);
    CHECK(pf_line_get_x1(a, h[3]) == 3);
    CHECK(pf_line_get_x1(a, h[4]) == 4);

    // order() holds exactly the live ids, oldest -> newest.
    const auto& ord = a.order();
    CHECK(ord.size() == (size_t)cap);
    CHECK(ord.front() == h[2].id);
    CHECK(ord.back() == h[4].id);
}

// ---------------------------------------------------------------------------
// get_price: linear interp + degenerate x1==x2 -> na + bar_time -> throw (spec §3.6)
static void test_get_price() {
    std::printf("test_get_price\n");
    DrawingArena<LineRec> a;
    // Line (0,10) -> (10,20): slope 1.0.
    Line h = pf_line_new(a, 0, 10.0, 10, 20.0, XLoc::bar_index);
    CHECK(near(pf_line_get_price(a, h, 0), 10.0));
    CHECK(near(pf_line_get_price(a, h, 5), 15.0));
    CHECK(near(pf_line_get_price(a, h, 10), 20.0));
    CHECK(near(pf_line_get_price(a, h, 20), 30.0));   // infinite line (ignores extend)
    CHECK(near(pf_line_get_price(a, h, -5), 5.0));

    // Degenerate vertical (x1 == x2) -> na.
    Line vert = pf_line_new(a, 5, 10.0, 5, 20.0, XLoc::bar_index);
    CHECK(is_na(pf_line_get_price(a, vert, 5)));

    // bar_time line -> throws (TV errors).
    Line tline = pf_line_new(a, 100, 10.0, 200, 20.0, XLoc::bar_time);
    CHECK_THROWS(pf_line_get_price(a, tline, 150));
}

// ---------------------------------------------------------------------------
// box: new (scalar + point), getters/setters, copy deep, delete, eviction
static void test_box() {
    std::printf("test_box\n");
    DrawingArena<BoxRec> a;
    Box b = pf_box_new(a, 1, 100.0, 5, 50.0);   // left,top,right,bottom
    CHECK(pf_box_get_left(a, b) == 1);
    CHECK(pf_box_get_right(a, b) == 5);
    CHECK(near(pf_box_get_top(a, b), 100.0));
    CHECK(near(pf_box_get_bottom(a, b), 50.0));

    pf_box_set_lefttop(a, b, 2, 110.0);
    pf_box_set_rightbottom(a, b, 6, 40.0);
    CHECK(pf_box_get_left(a, b) == 2);
    CHECK(near(pf_box_get_top(a, b), 110.0));
    CHECK(pf_box_get_right(a, b) == 6);
    CHECK(near(pf_box_get_bottom(a, b), 40.0));

    // point ctor: top_left / bottom_right via ChartPoint (bar_index axis).
    Box bp = pf_box_new_pts(a, ChartPoint{ 10, na<int64_t>(), 200.0 },
                               ChartPoint{ 20, na<int64_t>(), 150.0 });
    CHECK(pf_box_get_left(a, bp) == 10);
    CHECK(pf_box_get_right(a, bp) == 20);
    CHECK(near(pf_box_get_top(a, bp), 200.0));
    CHECK(near(pf_box_get_bottom(a, bp), 150.0));

    // deep copy independence
    Box cp = pf_box_copy(a, b);
    CHECK(cp.id != b.id);
    pf_box_set_top(a, cp, 777.0);
    CHECK(near(pf_box_get_top(a, b), 110.0));

    // delete -> getter throws; na-delete silent
    pf_box_delete(a, b);
    CHECK_THROWS(pf_box_get_top(a, b));
    CHECK_NOTHROW(pf_box_delete(a, Box{}));
}

// ---------------------------------------------------------------------------
// label: text get/set, copy deep (incl. string), delete, store-only yloc
static void test_label() {
    std::printf("test_label\n");
    DrawingArena<LabelRec> a;
    Label l = pf_label_new(a, 3, 42.0, "hello", XLoc::bar_index, YLoc::price);
    CHECK(pf_label_get_x(a, l) == 3);
    CHECK(near(pf_label_get_y(a, l), 42.0));
    CHECK(pf_label_get_text(a, l) == "hello");

    pf_label_set_text(a, l, "world");
    pf_label_set_xy(a, l, 9, 84.0);
    CHECK(pf_label_get_text(a, l) == "world");
    CHECK(pf_label_get_x(a, l) == 9);
    CHECK(near(pf_label_get_y(a, l), 84.0));

    // deep copy: mutating the copy's string leaves the original intact.
    Label cp = pf_label_copy(a, l);
    CHECK(cp.id != l.id);
    pf_label_set_text(a, cp, "changed");
    CHECK(pf_label_get_text(a, l) == "world");
    CHECK(pf_label_get_text(a, cp) == "changed");

    // na.new(na,na,na) -> LIVE record with na coords (jevondijefferson pattern).
    Label na_label = pf_label_new(a, na<int64_t>(), na<double>(), "");
    CHECK(!is_na(na_label));
    CHECK(is_na(pf_label_get_x(a, na_label)));

    // point ctor selects bar_time when only time is set.
    Label lt = pf_label_new_pt(a, ChartPoint{ na<int64_t>(), 1700000000000LL, 5.0 }, "t");
    CHECK(pf_label_get_x(a, lt) == 1700000000000LL);

    pf_label_delete(a, l);
    CHECK_THROWS(pf_label_get_text(a, l));
    CHECK_NOTHROW(pf_label_delete(a, l));   // double-delete silent
}

// ---------------------------------------------------------------------------
// linefill: stores two line ids, get_line1/2, delete; dead -> throw
static void test_linefill() {
    std::printf("test_linefill\n");
    DrawingArena<LineRec> la;
    Line l1 = pf_line_new(la, 0, 1.0, 1, 2.0);
    Line l2 = pf_line_new(la, 0, 3.0, 1, 4.0);

    DrawingArena<LinefillRec> fa;
    Linefill f = pf_linefill_new(fa, l1, l2);
    CHECK(!is_na(f));
    CHECK(pf_linefill_get_line1(fa, f).id == l1.id);
    CHECK(pf_linefill_get_line2(fa, f).id == l2.id);

    // linefill.new(na, na) is legal -> LIVE record over na line ids.
    Linefill fna = pf_linefill_new(fa, Line{}, Line{});
    CHECK(!is_na(fna));
    CHECK(is_na(pf_linefill_get_line1(fa, fna)));

    pf_linefill_delete(fa, f);
    CHECK_THROWS(pf_linefill_get_line1(fa, f));
    CHECK_NOTHROW(pf_linefill_delete(fa, f));     // double-delete silent
    CHECK_NOTHROW(pf_linefill_delete(fa, Linefill{}));
}

// ---------------------------------------------------------------------------
// chart.point value copy independence (spec §3.6: copy is plain by-value)
static void test_chart_point_value_copy() {
    std::printf("test_chart_point_value_copy\n");
    ChartPoint p1{ 1, 1000LL, 10.0 };
    ChartPoint p2 = p1;          // chart.point.copy(p) lowers to a by-value copy
    p2.price = 99.0;
    p2.index = 7;
    CHECK(near(p1.price, 10.0)); // original untouched
    CHECK(p1.index == 1);
    CHECK(near(p2.price, 99.0));
    CHECK(p2.index == 7);
}

// ---------------------------------------------------------------------------
// pf_noop accepts any args and returns void (spec §3.6)
static void test_pf_noop() {
    std::printf("test_pf_noop\n");
    CHECK_NOTHROW(pf_noop());
    CHECK_NOTHROW(pf_noop(1, 2.0, "three", Line{}));
    static_assert(std::is_same<decltype(pf_noop(1)), void>::value, "pf_noop returns void");
}

int main() {
    test_is_na_defaults();
    test_monotonic_ids();
    test_new_na_coords_is_live();
    test_mutate_through_handle();
    test_set_xloc_reinterpret();
    test_copy_is_deep();
    test_delete_then_getter_throws();
    test_fifo_eviction();
    test_get_price();
    test_box();
    test_label();
    test_linefill();
    test_chart_point_value_copy();
    test_pf_noop();

    if (g_fail > 0) {
        std::printf("drawing tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("drawing tests: all passed\n");
    return 0;
}
