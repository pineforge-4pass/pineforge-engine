/*
 * test_ta_pivot_point_levels.cpp — ta::PivotPointLevels, the pivot levels of
 * an anchored period, held or developing (R5 lane TA1).
 *
 * TradingView's Pine v6 reference for ta.pivot_point_levels(type, anchor,
 * developing):
 *   returns "An array<float> with numerical values representing 11 pivot
 *     point levels: [P, R1, S1, R2, S2, R3, S3, R4, S4, R5, S5]. Levels absent
 *     from the specified type return na values (e.g., "DM" only calculates P,
 *     R1, and S1)."
 *   type "Possible values: "Traditional", "Fibonacci", "Woodie", "Classic",
 *     "DM", "Camarilla"."
 *   anchor "The condition that triggers the reset of the pivot point
 *     calculations. When true, calculations reset; when false, results
 *     calculated at the last reset persist."
 *   developing "If false, the values are those calculated the last time the
 *     anchor condition was true. They remain constant until the anchor
 *     condition becomes true again. If true, the pivots are developing, i.e.,
 *     they constantly recalculate on the data developing between the point
 *     of the last anchor (or bar zero if the anchor condition was never true)
 *     and the current bar."
 *   Remarks "The developing parameter cannot be true when type is set to
 *     "Woodie", because the Woodie calculation for a period depends on that
 *     period's open, which means that the pivot value is either available or
 *     unavailable, but never developing. If used together, the indicator will
 *     return a runtime error."
 * The formulas are TradingView's Help Center "Pivot Points Standard" page
 * ("The "current" and "previous" values refer to the current and previous
 * bar of the timeframe selected"), transcribed below as reference_levels().
 *
 * Witness: every level of every bar against a naive recomputation -- the
 * period re-aggregated from its bars on every bar (first finite open, highest
 * finite high, lowest finite low, last finite close), the levels one rounded
 * operation per statement (this TU builds with -ffp-contract=off, as the
 * library does) -- bit for bit, for the six types, developing false and true
 * (Woodie false), per-bar type and developing switches, anchors on bar 0 /
 * never / every bar / at random / late, na inputs, and compute -> recompute ->
 * recompute sequences; the Woodie + developing runtime error and the type
 * names; and the free function ta::pivot_point_levels, which today's lowering
 * calls with the previous bar's high, low and close, and its six-argument
 * overload, which also takes that bar's open and this bar's, against the new
 * form anchored on every bar.
 */

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr, ...)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL %s:%d %s -- ", __FILE__, __LINE__, #expr);     \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool same(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return std::memcmp(&a, &b, sizeof a) == 0;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    double unit() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

static const char* const kTypes[] = {"Traditional", "Fibonacci", "Woodie", "Classic", "DM", "Camarilla"};

// ---------------------------------------------------------------------------
// The reference: the Help Center's formulas, written from the page.
// ---------------------------------------------------------------------------
struct Period { double open, high, low, close; };  // na where the period has none

static std::vector<double> reference_levels(const std::string& type, const Period& prev,
                                            double curr_open) {
    std::vector<double> out(11, na<double>());
    const double prevHigh = prev.high, prevLow = prev.low, prevClose = prev.close;
    const double prevOpen = prev.open;
    auto set = [&](int i, double v) { out[i] = v; };
    if (type == "Woodie") {
        if (is_na(prevHigh) || is_na(prevLow) || is_na(curr_open)) return out;
        const double P = (prevHigh + prevLow + 2 * curr_open) / 4;
        const double R1 = 2 * P - prevLow;
        const double S1 = 2 * P - prevHigh;
        const double R2 = P + (prevHigh - prevLow);
        const double S2 = P - (prevHigh - prevLow);
        const double R3 = prevHigh + 2 * (P - prevLow);
        const double S3 = prevLow - 2 * (prevHigh - P);
        const double R4 = R3 + (prevHigh - prevLow);
        const double S4 = S3 - (prevHigh - prevLow);
        set(0, P); set(1, R1); set(2, S1); set(3, R2); set(4, S2);
        set(5, R3); set(6, S3); set(7, R4); set(8, S4);
        return out;
    }
    if (type == "DM") {
        if (is_na(prevOpen) || is_na(prevHigh) || is_na(prevLow) || is_na(prevClose)) return out;
        double X;
        if (prevOpen == prevClose) X = prevHigh + prevLow + 2 * prevClose;
        else if (prevClose > prevOpen) X = 2 * prevHigh + prevLow + prevClose;
        else X = 2 * prevLow + prevHigh + prevClose;
        set(0, X / 4); set(1, X / 2 - prevLow); set(2, X / 2 - prevHigh);
        return out;
    }
    if (is_na(prevHigh) || is_na(prevLow) || is_na(prevClose)) return out;
    const double P = (prevHigh + prevLow + prevClose) / 3;
    set(0, P);
    if (type == "Traditional") {
        set(1, P * 2 - prevLow);
        set(2, P * 2 - prevHigh);
        set(3, P + (prevHigh - prevLow));
        set(4, P - (prevHigh - prevLow));
        set(5, P * 2 + (prevHigh - 2 * prevLow));
        set(6, P * 2 - (2 * prevHigh - prevLow));
        set(7, P * 3 + (prevHigh - 3 * prevLow));
        set(8, P * 3 - (3 * prevHigh - prevLow));
        set(9, P * 4 + (prevHigh - 4 * prevLow));
        set(10, P * 4 - (4 * prevHigh - prevLow));
    } else if (type == "Fibonacci") {
        set(1, P + 0.382 * (prevHigh - prevLow));
        set(2, P - 0.382 * (prevHigh - prevLow));
        set(3, P + 0.618 * (prevHigh - prevLow));
        set(4, P - 0.618 * (prevHigh - prevLow));
        set(5, P + (prevHigh - prevLow));
        set(6, P - (prevHigh - prevLow));
    } else if (type == "Classic") {
        set(1, 2 * P - prevLow);
        set(2, 2 * P - prevHigh);
        set(3, P + (prevHigh - prevLow));
        set(4, P - (prevHigh - prevLow));
        set(5, P + 2 * (prevHigh - prevLow));
        set(6, P - 2 * (prevHigh - prevLow));
        set(7, P + 3 * (prevHigh - prevLow));
        set(8, P - 3 * (prevHigh - prevLow));
    } else if (type == "Camarilla") {
        set(1, prevClose + 1.1 * (prevHigh - prevLow) / 12);
        set(2, prevClose - 1.1 * (prevHigh - prevLow) / 12);
        set(3, prevClose + 1.1 * (prevHigh - prevLow) / 6);
        set(4, prevClose - 1.1 * (prevHigh - prevLow) / 6);
        set(5, prevClose + 1.1 * (prevHigh - prevLow) / 4);
        set(6, prevClose - 1.1 * (prevHigh - prevLow) / 4);
        set(7, prevClose + 1.1 * (prevHigh - prevLow) / 2);
        set(8, prevClose - 1.1 * (prevHigh - prevLow) / 2);
        // R5 = (prevHigh / prevLow) * prevClose; na on a zero low, as the
        // free function has it.
        const double R5 = prevLow != 0.0 ? (prevHigh / prevLow) * prevClose : na<double>();
        set(9, R5);
        set(10, is_na(R5) ? na<double>() : prevClose - (R5 - prevClose));
    }
    return out;
}

struct Feed {
    std::vector<double> o, h, l, c;
};

static Feed make_feed(int n, uint64_t seed, bool with_na) {
    Rng r(seed);
    Feed f;
    double close = 2500.0;
    for (int i = 0; i < n; ++i) {
        // Opens that sometimes equal the previous close and sometimes gap,
        // and closes that sometimes equal their open (DM's third branch).
        double open = r.unit() < 0.5 ? close : close + (r.unit() - 0.5) * 6.0;
        double c = r.unit() < 0.1 ? open : open + (r.unit() - 0.5) * 20.0;
        double h = std::max(open, c) + std::floor(r.unit() * 8.0) * 0.25;
        double l = std::min(open, c) - std::floor(r.unit() * 8.0) * 0.25;
        close = c;
        if (with_na) {
            const double u = r.unit();
            if (u < 0.01) open = na<double>();
            else if (u < 0.02) h = na<double>();
            else if (u < 0.03) l = na<double>();
            else if (u < 0.04) c = na<double>();
        }
        f.o.push_back(open);
        f.h.push_back(h);
        f.l.push_back(l);
        f.c.push_back(c);
    }
    return f;
}

// The aggregate of bars [from, to] (empty when from > to), re-read naively.
static Period aggregate(const Feed& f, int from, int to) {
    Period p{na<double>(), na<double>(), na<double>(), na<double>()};
    for (int k = from; k <= to; ++k) {
        if (is_na(p.open) && !is_na(f.o[k])) p.open = f.o[k];
        if (!is_na(f.h[k]) && (is_na(p.high) || f.h[k] > p.high)) p.high = f.h[k];
        if (!is_na(f.l[k]) && (is_na(p.low) || f.l[k] < p.low)) p.low = f.l[k];
        if (!is_na(f.c[k])) p.close = f.c[k];
    }
    return p;
}

// The reference's answer on bar t.
static std::vector<double> reference(const Feed& f, const std::vector<char>& anchor,
                                     const std::vector<int>& type, const std::vector<char>& dev,
                                     int t) {
    int last = -1;
    for (int k = t; k >= 0; --k) if (anchor[k]) { last = k; break; }
    if (dev[t]) {
        // The data developing between the last anchor (or bar zero) and now.
        const int from = last < 0 ? 0 : last;
        return reference_levels(kTypes[type[t]], aggregate(f, from, t), na<double>());
    }
    if (last < 0) return std::vector<double>(11, na<double>());
    // Held: the levels calculated on the last anchored bar, of the period
    // that bar closed -- from the anchor before it (or bar zero) to the bar
    // before it -- with that bar's type and, for Woodie, its open.
    int before = -1;
    for (int k = last - 1; k >= 0; --k) if (anchor[k]) { before = k; break; }
    const int from = before < 0 ? 0 : before;
    return reference_levels(kTypes[type[last]], aggregate(f, from, last - 1), f.o[last]);
}

static bool same_levels(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != 11 || b.size() != 11) return false;
    for (int i = 0; i < 11; ++i) if (!same(a[i], b[i])) return false;
    return true;
}

static std::vector<char> anchors(int n, const char* kind, uint64_t seed) {
    std::vector<char> a(n, 0);
    Rng r(seed);
    for (int i = 0; i < n; ++i) {
        if (!std::strcmp(kind, "bar0")) a[i] = i == 0;
        else if (!std::strcmp(kind, "every")) a[i] = 1;
        else if (!std::strcmp(kind, "random")) a[i] = i >= 30 && r.unit() < 0.05;
        else if (!std::strcmp(kind, "periodic")) a[i] = i % 96 == 17;
        else if (!std::strcmp(kind, "late")) a[i] = i == n - 4;
    }
    if (!std::strcmp(kind, "random")) a[30] = 1;
    return a;
}

static void run(const Feed& f, const std::vector<char>& anchor, const std::vector<int>& type,
                const std::vector<char>& dev, const char* label) {
    ta::PivotPointLevels by_name, by_enum;
    int bad = 0, valued = 0;
    for (int t = 0; t < (int)f.o.size(); ++t) {
        const std::vector<double> want = reference(f, anchor, type, dev, t);
        const std::vector<double> got = by_name.compute(kTypes[type[t]], anchor[t] != 0, dev[t] != 0,
                                                        f.o[t], f.h[t], f.l[t], f.c[t]);
        const std::vector<double> got_e = by_enum.compute(ta::pivot_levels_type(kTypes[type[t]]),
                                                          anchor[t] != 0, dev[t] != 0,
                                                          f.o[t], f.h[t], f.l[t], f.c[t]);
        if (!is_na(want[0])) ++valued;
        if (!same_levels(got, want) || !same_levels(got_e, want)) {
            if (bad++ < 3) {
                std::printf("  [%s] bar %d type %s dev %d anchor %d:", label, t, kTypes[type[t]],
                            int(dev[t]), int(anchor[t]));
                for (int i = 0; i < 11; ++i) std::printf(" %.17g/%.17g", got[i], want[i]);
                std::printf("\n");
            }
        }
    }
    CHECK(bad == 0, "[%s] %d mismatching bars", label, bad);
    (void)valued;
}

static void test_against_reference() {
    std::printf("test_against_reference\n");
    for (bool with_na : {false, true}) {
        const Feed f = make_feed(1500, with_na ? 0x9e01ULL : 0x9e02ULL, with_na);
        const int n = (int)f.o.size();
        for (const char* kind : {"bar0", "never", "every", "random", "periodic", "late"}) {
            const std::vector<char> a = anchors(n, kind, 3);
            for (int ty = 0; ty < 6; ++ty) {
                for (int d = 0; d < 2; ++d) {
                    if (ty == 2 && d == 1) continue;  // Woodie is never developing
                    char label[96];
                    std::snprintf(label, sizeof label, "%s/%s/dev=%d/na=%d", kind, kTypes[ty], d,
                                  int(with_na));
                    run(f, a, std::vector<int>(n, ty), std::vector<char>(n, char(d)), label);
                }
            }
            // A series type and a series developing flag (never Woodie while
            // developing): held levels keep the type of their anchored bar.
            Rng r(0x51ULL);
            std::vector<int> type(n);
            std::vector<char> dev(n);
            for (int t = 0; t < n; ++t) {
                type[t] = int(r.unit() * 6.0);
                dev[t] = type[t] != 2 && r.unit() < 0.5;
            }
            char label[96];
            std::snprintf(label, sizeof label, "%s/series/na=%d", kind, int(with_na));
            run(f, a, type, dev, label);
        }
    }
}

// Pinned by hand on a three-bar period: H 110, L 100, O 101, C 108; the next
// period opens at 107.
static void test_literal_period() {
    std::printf("test_literal_period\n");
    ta::PivotPointLevels trad, wood, dm, dev;
    const bool A = true, F = false;
    // Bars 0-2 form the period (anchor on bar 0), bar 3 anchors the next one.
    const double O[] = {101, 103, 104, 107}, H[] = {105, 110, 109, 108};
    const double L[] = {100, 102, 103, 106}, C[] = {103, 104, 108, 107.5};
    const bool anc[] = {A, F, F, A};
    std::vector<double> t3, w3, d3, dv;
    for (int i = 0; i < 4; ++i) {
        t3 = trad.compute("Traditional", anc[i], F, O[i], H[i], L[i], C[i]);
        w3 = wood.compute("Woodie", anc[i], F, O[i], H[i], L[i], C[i]);
        d3 = dm.compute("DM", anc[i], F, O[i], H[i], L[i], C[i]);
        dv = dev.compute("Classic", anc[i], true, O[i], H[i], L[i], C[i]);
        if (i < 3) CHECK(is_na(t3[0]) && is_na(w3[0]) && is_na(d3[0]), "bar %d held before a close", i);
        if (i == 2) {
            // Developing on bar 2: the period so far, H 110 L 100 C 108.
            const double P = (110.0 + 100.0 + 108.0) / 3;
            CHECK(same(dv[0], P) && same(dv[1], 2 * P - 100.0), "bar 2 developing %.17g", dv[0]);
        }
    }
    const double P = (110.0 + 100.0 + 108.0) / 3;
    CHECK(same(t3[0], P) && same(t3[1], P * 2 - 100) && same(t3[2], P * 2 - 110),
          "traditional P/R1/S1 %.17g %.17g %.17g", t3[0], t3[1], t3[2]);
    CHECK(same(t3[7], P * 3 + (110 - 3 * 100.0)) && same(t3[10], P * 4 - (4 * 110.0 - 100)),
          "traditional R4 %.17g S5 %.17g", t3[7], t3[10]);
    const double Pw = (110.0 + 100.0 + 2 * 107.0) / 4;   // the NEW period's open
    CHECK(same(w3[0], Pw) && same(w3[5], 110 + 2 * (Pw - 100)), "woodie P %.17g R3 %.17g", w3[0], w3[5]);
    CHECK(is_na(w3[9]) && is_na(w3[10]), "woodie has no R5/S5");
    const double X = 2 * 110.0 + 100.0 + 108.0;          // close 108 > open 101
    CHECK(same(d3[0], X / 4) && same(d3[1], X / 2 - 100) && same(d3[2], X / 2 - 110),
          "dm %.17g %.17g %.17g", d3[0], d3[1], d3[2]);
    for (int i = 3; i < 11; ++i) CHECK(is_na(d3[i]), "dm slot %d", i);
    // Developing on the anchored bar 3: the new period is bar 3 alone.
    const double P3 = (108.0 + 106.0 + 107.5) / 3;
    CHECK(same(dv[0], P3), "developing restarts on the anchor: %.17g want %.17g", dv[0], P3);
}

// compute(first tick) -> recompute(second tick) -> recompute(final values):
// the bar and every later bar answer the reference over the final values,
// whatever the discarded ticks said (their anchor, type and developing flag
// included).
static void test_recompute_sequences() {
    std::printf("test_recompute_sequences\n");
    const Feed f = make_feed(900, 0x9e03ULL, true);
    const int n = (int)f.o.size();
    for (const char* kind : {"random", "every", "never"}) {
        const std::vector<char> a = anchors(n, kind, 5);
        Rng r(0x61ULL);
        std::vector<int> type(n);
        std::vector<char> dev(n);
        for (int t = 0; t < n; ++t) {
            type[t] = int(r.unit() * 6.0);
            dev[t] = type[t] != 2 && r.unit() < 0.5;
        }
        ta::PivotPointLevels p;
        int bad = 0;
        for (int t = 0; t < n; ++t) {
            const int t1 = int(r.unit() * 6.0), t2 = int(r.unit() * 6.0);
            // One draw per statement, left to right: C++ leaves the order in
            // which a call's arguments are evaluated unspecified.
            const bool anchor1 = r.unit() < 0.5;
            const bool developing1 = t1 != 2 && r.unit() < 0.5;
            p.compute(kTypes[t1], anchor1, developing1, 2400.0, 2600.0, 2300.0, 2450.0);
            const bool anchor2 = r.unit() < 0.5;
            const bool developing2 = t2 != 2 && r.unit() < 0.5;
            const double open2 = r.unit() < 0.2 ? na<double>() : 2410.0;
            p.recompute(kTypes[t2], anchor2, developing2, open2, 2700.0, 2200.0, 2500.0);
            const std::vector<double> got =
                p.recompute(kTypes[type[t]], a[t] != 0, dev[t] != 0, f.o[t], f.h[t], f.l[t], f.c[t]);
            if (!same_levels(got, reference(f, a, type, dev, t))) ++bad;
        }
        CHECK(bad == 0, "[%s] %d mismatching bars after recompute sequences", kind, bad);
    }
    ta::PivotPointLevels early, fresh;
    CHECK(same_levels(early.recompute("Classic", true, true, 1, 3, 0.5, 2),
                      fresh.compute("Classic", true, true, 1, 3, 0.5, 2)),
          "recompute before the first compute");
}

static void test_woodie_developing_and_names() {
    std::printf("test_woodie_developing_and_names\n");
    ta::PivotPointLevels p;
    bool threw = false;
    std::string what;
    try {
        p.compute("Woodie", true, true, 1, 2, 0.5, 1.5);
    } catch (const std::runtime_error& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw && what.find("Woodie") != std::string::npos, "Woodie + developing: threw=%d '%s'",
          int(threw), what.c_str());
    threw = false;
    try {
        ta::PivotPointLevels q;
        q.compute(ta::PivotLevelsType::Woodie, false, false, 1, 2, 0.5, 1.5);
        q.recompute(ta::PivotLevelsType::Woodie, false, true, 1, 2, 0.5, 1.5);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "Woodie + developing on recompute");
    // Woodie held (developing false) is fine.
    ta::PivotPointLevels ok;
    ok.compute("Woodie", true, false, 1, 2, 0.5, 1.5);
    ok.compute("Woodie", true, false, 1.2, 2, 0.5, 1.5);
    CHECK(true, "Woodie held");
    // The six names, and nothing else.
    CHECK(ta::pivot_levels_type("Traditional") == ta::PivotLevelsType::Traditional, "Traditional");
    CHECK(ta::pivot_levels_type("Fibonacci") == ta::PivotLevelsType::Fibonacci, "Fibonacci");
    CHECK(ta::pivot_levels_type("Woodie") == ta::PivotLevelsType::Woodie, "Woodie");
    CHECK(ta::pivot_levels_type("Classic") == ta::PivotLevelsType::Classic, "Classic");
    CHECK(ta::pivot_levels_type("DM") == ta::PivotLevelsType::DM, "DM");
    CHECK(ta::pivot_levels_type("Camarilla") == ta::PivotLevelsType::Camarilla, "Camarilla");
    for (const char* bad : {"traditional", "Demark", "", "Woodie "}) {
        bool refused = false;
        try { (void)ta::pivot_levels_type(bad); } catch (const std::invalid_argument&) { refused = true; }
        CHECK(refused, "type name '%s' refused", bad);
    }
}

// Today's lowering: ta::pivot_point_levels(type, high[1], low[1], close[1]).
// The new form anchored on every bar (developing false) closes a one-bar
// period on every bar -- the previous bar -- so it computes the same levels
// wherever the free function spells the same formula as the reference, and
// the reference's own values elsewhere. Pinned per slot, on the quarter-tick
// feed and on a wide off-tick one: equal on every bar for every slot of
// Traditional, Fibonacci, Classic and Camarilla (lane B-ENGINE put
// Traditional's R3..S5 in the reference's operation order; on 8cf3be58 slots
// 7..10 differed on both feeds and 5..6 on the wide one); Woodie and DM,
// whose inputs this signature does not carry, are counted and printed. The
// six-argument free function also takes the previous bar's open and this
// bar's open, and equals the new form in every slot of every type.
// A feed whose bars sit on no tick and whose high is up to forty times its
// low: there the two operation orders of Traditional's R3 and S3 round
// apart (on the quarter-tick feed above they never do; R3's need a low
// under a quarter of the high).
static Feed make_wide_feed(int n, uint64_t seed) {
    Rng r(seed);
    Feed f;
    for (int i = 0; i < n; ++i) {
        const double base = 10.0 + r.unit() * 990.0;
        const double h = base * (1.0 + r.unit() * 3.0);
        const double l = base * (0.1 + r.unit() * 0.9);
        f.o.push_back(l + r.unit() * (h - l));
        f.h.push_back(h);
        f.l.push_back(l);
        f.c.push_back(l + r.unit() * (h - l));
    }
    return f;
}

static void free_function_equivalence_on(const Feed& f, const char* feed) {
    std::printf("  %s feed\n", feed);
    const int n = (int)f.o.size();
    for (int ty = 0; ty < 6; ++ty) {
        ta::PivotPointLevels p;
        int differ[11] = {0};
        int differ_full[11] = {0};
        for (int t = 0; t < n; ++t) {
            const std::vector<double> got = p.compute(kTypes[ty], true, false, f.o[t], f.h[t], f.l[t], f.c[t]);
            const double O = t ? f.o[t - 1] : na<double>();
            const double H = t ? f.h[t - 1] : na<double>();
            const double L = t ? f.l[t - 1] : na<double>();
            const double C = t ? f.c[t - 1] : na<double>();
            const std::vector<double> free_fn = ta::pivot_point_levels(kTypes[ty], H, L, C);
            const std::vector<double> full_fn = ta::pivot_point_levels(kTypes[ty], O, H, L, C, f.o[t]);
            for (int i = 0; i < 11; ++i) if (!same(got[i], free_fn[i])) ++differ[i];
            for (int i = 0; i < 11; ++i) if (!same(got[i], full_fn[i])) ++differ_full[i];
        }
        std::printf("  %-11s differing bars per slot [P R1 S1 R2 S2 R3 S3 R4 S4 R5 S5]:", kTypes[ty]);
        for (int i = 0; i < 11; ++i) std::printf(" %d", differ[i]);
        std::printf("; six-argument:");
        for (int i = 0; i < 11; ++i) std::printf(" %d", differ_full[i]);
        std::printf("\n");
        const std::string type = kTypes[ty];
        int must_equal = 0;
        if (type == "Traditional" || type == "Fibonacci" || type == "Classic"
            || type == "Camarilla") must_equal = 11;
        for (int i = 0; i < must_equal; ++i)
            CHECK(differ[i] == 0, "%s slot %d differs from the free function on %d bars",
                  kTypes[ty], i, differ[i]);
        for (int i = 0; i < 11; ++i)
            CHECK(differ_full[i] == 0,
                  "%s slot %d differs from the six-argument free function on %d bars",
                  kTypes[ty], i, differ_full[i]);
    }
}

static void test_free_function_equivalence() {
    std::printf("test_free_function_equivalence\n");
    free_function_equivalence_on(make_feed(3000, 0x9e04ULL, false), "quarter-tick");
    free_function_equivalence_on(make_wide_feed(3000, 0x51deULL), "wide off-tick");
    // The six-argument form's na rule is the reference's: Woodie needs no
    // close and DM no next open, and a missing input a formula reads leaves
    // all eleven na.
    const std::vector<double> woodie = ta::pivot_point_levels("Woodie", na<double>(), 2.0, 0.5,
                                                              na<double>(), 1.25);
    CHECK(!is_na(woodie[0]) && !is_na(woodie[8]) && is_na(woodie[9]),
          "Woodie from high, low and the next open");
    const std::vector<double> dm = ta::pivot_point_levels("DM", 1.0, 2.0, 0.5, 1.5, na<double>());
    CHECK(!is_na(dm[0]) && !is_na(dm[2]) && is_na(dm[3]), "DM from the period alone");
    const std::vector<double> dm_no_open = ta::pivot_point_levels("DM", na<double>(), 2.0, 0.5,
                                                                  1.5, 1.25);
    bool all_na = true;
    for (double v : dm_no_open) all_na = all_na && is_na(v);
    CHECK(all_na, "DM without the period's open is all na");
    bool refused = false;
    try {
        (void)ta::pivot_point_levels("Demark", 1.0, 2.0, 0.5, 1.5, 1.25);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CHECK(refused, "six-argument form refuses an unknown type name");
}

// Lane C7's TradingView export (codegen tests/fixtures/c7_tv_evidence, the
// BINANCE:ETHUSDT.P 15-minute probe): the first period O=1821.59 H=1842.57
// L=1816.64 C=1837.47, the next period opening at 1837.48, and every finite
// level TradingView printed, cut to four decimals by the quantity field. The
// six-argument free function reproduces each within that cut, and so does the
// four-argument one for Traditional, Fibonacci, Classic and Camarilla.
static void test_c7_tradingview_period() {
    std::printf("test_c7_tradingview_period\n");
    const double N = na<double>();
    struct Tape {
        const char* type;
        double tv[11];
    };
    const Tape tapes[] = {
        {"Traditional", {1832.2266, 1847.8133, 1821.8833, 1858.1566, 1806.2966, 1873.7433,
                         1795.9533, 1889.3300, 1785.6100, 1904.9166, 1775.2666}},
        {"Fibonacci", {1832.2266, 1842.1319, 1822.3214, 1848.2514, 1816.2019, 1858.1566,
                       1806.2966, N, N, N, N}},
        {"Woodie", {1833.5425, 1850.4450, 1824.5150, 1859.4725, 1807.6125, 1876.3750,
                    1798.5850, 1902.3050, 1772.6550, N, N}},
        {"Classic", {1832.2266, 1847.8133, 1821.8833, 1858.1566, 1806.2966, 1884.0866,
                     1780.3666, 1910.0166, 1754.4366, N, N}},
        {"DM", {1834.8125, 1852.9850, 1827.0550, N, N, N, N, N, N, N, N}},
        {"Camarilla", {1832.2266, 1839.8469, 1835.0930, 1842.2238, 1832.7161, 1844.6007,
                       1830.3392, 1851.7315, 1823.2085, 1863.6973, 1811.2426}},
    };
    const auto within_cut = [](double level, double tv) {
        if (is_na(tv)) return is_na(level);
        return !is_na(level) && std::fabs(level - tv) < 1.0e-4 + 1.0e-9;
    };
    for (const auto& tape : tapes) {
        const std::string type = tape.type;
        const std::vector<double> full =
            ta::pivot_point_levels(type, 1821.59, 1842.57, 1816.64, 1837.47, 1837.48);
        const std::vector<double> hlc = ta::pivot_point_levels(type, 1842.57, 1816.64, 1837.47);
        const bool hlc_is_tv = type != "Woodie" && type != "DM";
        for (int i = 0; i < 11; ++i) {
            CHECK(within_cut(full[i], tape.tv[i]), "%s slot %d: %.6f against TradingView %.4f",
                  tape.type, i, full[i], tape.tv[i]);
            if (hlc_is_tv)
                CHECK(within_cut(hlc[i], tape.tv[i]),
                      "%s slot %d (four-argument): %.6f against TradingView %.4f", tape.type, i,
                      hlc[i], tape.tv[i]);
        }
    }
}

int main() {
    test_against_reference();
    test_literal_period();
    test_recompute_sequences();
    test_woodie_developing_and_names();
    test_free_function_equivalence();
    test_c7_tradingview_period();
    std::printf("test_ta_pivot_point_levels: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
