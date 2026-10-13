/*
 * test_ta_extremes_ring.cpp — TradingView's bar-addressed ring for window
 * extremes and the per-context bar index installed by the engine.
 *
 * A call site owns K = length + 1 slots addressed by bar_index % K. Calls
 * write the current slot; stale values survive, unwritten slots read as 0.
 * Warmup uses bar_index - origin, not the number of executions. A cached
 * extremum retains its implied bar until expiry; a new extremum wins before
 * the expiry rescan. Written na poisons the cache and the older window.
 *
 * Pinned against TradingView behaviour (recorded tapes are kept outside
 * this repository). Synthetic cadence fixtures preserve slot aliasing,
 * overwrites and offsets; every-bar callers are compared with a deque.
 * NDEBUG-PROOF: CHECK increments a failure counter and main returns nonzero.
 */

#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

// Bitwise-equal-or-both-na: the identity the every-bar proofs demand.
static bool same(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return a == b;
}

// Invented highs: only cadence call sites need designed geometry; other
// bars follow a short periodic formula. Values are not a transformed tape.
static double synthetic_high(int b) {
    switch (b) {
        case 3: return 120.0;
        case 10: return 110.0;
        case 17: return 135.0;
        case 24: return 150.0;
        case 31: return 170.0;
        case 38: return 140.0;
        case 45: return 160.0;
        case 52: return 130.0;
        case 59: return 155.0;
        case 66: return 210.0;
        case 73: return 190.0;
        case 80: return 180.0;
        case 87: return 195.0;
        case 12: return 115.0;
        case 21: return 145.0;
        case 30: return 165.0;
        case 39: return 125.0;
        case 48: return 150.0;
        case 57: return 175.0;
        case 75: return 200.0;
        case 84: return 185.0;
        default: return 100.0 + (b % 8) * 2.0;
    }
}
static const int kNumHighs = 90;

// Expected sizes encode round(extremum * 100), with 1 for warmup na.
// Cadence 7: 120 survives bar 10, new peaks 135/150/170 follow; peak 170
// remains in slot 1 at offsets 1..4, then bar 66's 210 replaces it.
// Cadence 9: slots 3 and 0 alternate; at bar 48, 165 in slot 0 is
// overwritten by 150 while slot 3 holds 125, hence max(150,125) = 150.
static const int kTvSizesCadence7[13] = {1, 12000, 13500, 15000, 17000, 17000,
                                       17000, 17000, 17000, 21000, 21000, 21000, 21000};
static const int kTvSizesCadence9[10] = {1, 12000, 14500, 16500, 16500, 15000,
                                       17500, 21000, 21000, 20000};

static int tv_size(double v) {
    return is_na(v) ? 1 : static_cast<int>(std::llround(v * 100.0));
}

// --- The previous implementation, kept as the every-bar reference ---

// In an oldest-first positional scan, na resets the running extremum.
// Only members newer than the newest na remain live; none means na.
// The pre-poison deque incorrectly considered all non-na members.
static double ref_extremum(const std::deque<double>& buffer, bool want_max) {
    double best = na<double>();
    for (double v : buffer) {
        if (is_na(v)) { best = na<double>(); continue; }
        if (is_na(best) || (want_max ? v > best : v < best)) best = v;
    }
    return best;
}

struct RefHighest {
    std::deque<double> buffer;
    int length;
    explicit RefHighest(int len) : length(len) {}
    double compute(double src) {
        buffer.push_back(src);
        while ((int)buffer.size() > length) buffer.pop_front();
        if ((int)buffer.size() < length) return na<double>();
        if (is_na(src)) return na<double>();   // an na source answers na
        return ref_extremum(buffer, true);
    }
    double recompute(double src) {
        if (buffer.empty()) return compute(src);
        buffer.back() = src;
        if ((int)buffer.size() < length) return na<double>();
        if (is_na(src)) return na<double>();
        return ref_extremum(buffer, true);
    }
};

struct RefLowest {
    std::deque<double> buffer;
    int length;
    explicit RefLowest(int len) : length(len) {}
    double compute(double src) {
        buffer.push_back(src);
        while ((int)buffer.size() > length) buffer.pop_front();
        if ((int)buffer.size() < length) return na<double>();
        if (is_na(src)) return na<double>();   // an na source answers na
        return ref_extremum(buffer, false);
    }
    double recompute(double src) {
        if (buffer.empty()) return compute(src);
        buffer.back() = src;
        if ((int)buffer.size() < length) return na<double>();
        if (is_na(src)) return na<double>();
        return ref_extremum(buffer, false);
    }
};

struct RefHighestBars {
    int length_;
    std::deque<double> buffer_;
    explicit RefHighestBars(int len) : length_(len) {}
    double compute(double src) {
        if (is_na(src)) return na<double>();
        buffer_.push_back(src);
        while ((int)buffer_.size() > length_) buffer_.pop_front();
        if ((int)buffer_.size() < length_) return na<double>();
        int max_idx = 0;
        double max_val = buffer_[0];
        for (int i = 1; i < (int)buffer_.size(); i++)
            if (buffer_[i] > max_val) { max_val = buffer_[i]; max_idx = i; }
        return (double)(max_idx - ((int)buffer_.size() - 1));
    }
    double recompute(double src) {
        if (buffer_.empty()) return compute(src);
        buffer_.back() = src;
        if (is_na(src)) return na<double>();
        if ((int)buffer_.size() < length_) return na<double>();
        int max_idx = 0;
        double max_val = buffer_[0];
        for (int i = 1; i < (int)buffer_.size(); i++)
            if (buffer_[i] > max_val) { max_val = buffer_[i]; max_idx = i; }
        return (double)(max_idx - ((int)buffer_.size() - 1));
    }
};

struct RefLowestBars {
    int length_;
    std::deque<double> buffer_;
    explicit RefLowestBars(int len) : length_(len) {}
    double compute(double src) {
        if (is_na(src)) return na<double>();
        buffer_.push_back(src);
        while ((int)buffer_.size() > length_) buffer_.pop_front();
        if ((int)buffer_.size() < length_) return na<double>();
        int min_idx = 0;
        double min_val = buffer_[0];
        for (int i = 1; i < (int)buffer_.size(); i++)
            if (buffer_[i] < min_val) { min_val = buffer_[i]; min_idx = i; }
        return (double)(min_idx - ((int)buffer_.size() - 1));
    }
    double recompute(double src) {
        if (buffer_.empty()) return compute(src);
        buffer_.back() = src;
        if (is_na(src)) return na<double>();
        if ((int)buffer_.size() < length_) return na<double>();
        int min_idx = 0;
        double min_val = buffer_[0];
        for (int i = 1; i < (int)buffer_.size(); i++)
            if (buffer_[i] < min_val) { min_val = buffer_[i]; min_idx = i; }
        return (double)(min_idx - ((int)buffer_.size() - 1));
    }
};

// Deterministic LCG series; `na_every` > 0 plants an na gap every that many
// bars (0 = gap-free).
static std::vector<double> make_series(int n, unsigned seed, int na_every) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(n));
    unsigned x = seed;
    for (int i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        double v = 100.0 + static_cast<double>((x >> 8) % 100000) / 1000.0;
        if (na_every > 0 && (i % na_every) == na_every - 1) v = na<double>();
        out.push_back(v);
    }
    return out;
}

// --- Designed cadence pins, with chart bar addressing ---

static void test_cadence7_pin() {
    std::printf("test_cadence7_pin\n");
    ta::Highest hi(5);
    int call = 0;
    for (int b = 0; b < kNumHighs; ++b) {
        ta::BarContextScope scope(b, 0);
        if (b % 7 != 3) continue;
        double v = hi.compute(synthetic_high(b));
        CHECK(call < 13);
        if (call < 13) CHECK(tv_size(v) == kTvSizesCadence7[call]);
        if (call == 0) CHECK(is_na(v));                 // bar 3 < length - 1 = 4
        if (call == 1) CHECK(near(v, 120.0));            // max{bar 3, bar 10}: 2 executions, not na
        if (call == 5) CHECK(near(v, 170.0));           // bar 31's 170 survives in slot 1
        ++call;
    }
    CHECK(call == 13);
    CHECK(!ta::bar_context().installed);   // every scope restored on exit
}

static void test_cadence9_pin() {
    std::printf("test_cadence9_pin\n");
    ta::Highest hi(5);
    int call = 0;
    for (int b = 0; b < kNumHighs; ++b) {
        ta::BarContextScope scope(b, 0);
        if (b % 9 != 3) continue;
        double v = hi.compute(synthetic_high(b));
        CHECK(call < 10);
        if (call < 10) CHECK(tv_size(v) == kTvSizesCadence9[call]);
        // Bar 48 overwrites slot 0's 165 with 150; slot 3 holds 125.
        // The ring returns max(150,125)=150; a per-call window keeps 165.
        if (call == 5) CHECK(near(v, 150.0));
        ++call;
    }
    CHECK(call == 10);
}

// Evaluate the guard in chart context, including bars that make no call.
// Negating the designed highs preserves the lowest/highest mirror.
static void test_cadence_pin_lowest_mirror() {
    std::printf("test_cadence_pin_lowest_mirror\n");
    for (int cadence : {7, 9}) {
        ta::Highest hi(5);
        ta::Lowest lo(5);
        for (int b = 0; b < kNumHighs; ++b) {
            ta::BarContextScope scope(b, 0);
            if (b % cadence != 3) continue;
            double h = hi.compute(synthetic_high(b));
            double l = lo.compute(-synthetic_high(b));
            CHECK(same(l, is_na(h) ? h : -h));   // lowest(-x) == -highest(x)
        }
    }
}

// --- na iff bar_index < length - 1 (never "until length executions") ---

static void test_na_until_bar_length_minus_1() {
    std::printf("test_na_until_bar_length_minus_1\n");
    // A caller executing on bars 0..3 of a length-5 window: na on every one,
    // then bar 4 answers from the written slots even though bar 4 is only
    // the 2nd execution of the second object below.
    {
        ta::Highest every(5);
        for (int b = 0; b < 4; ++b) {
            ta::BarContextScope scope(b, 0);
            CHECK(is_na(every.compute(1.0 + b)));
        }
        ta::BarContextScope scope(4, 0);
        CHECK(near(every.compute(0.5), 4.0));
    }
    {
        ta::Highest sparse(5);
        { ta::BarContextScope scope(2, 0); CHECK(is_na(sparse.compute(7.0))); }
        { ta::BarContextScope scope(4, 0); CHECK(near(sparse.compute(3.0), 7.0)); }  // max{bar 2, bar 4}
        { ta::BarContextScope scope(5, 0); CHECK(near(sparse.compute(2.0), 7.0)); }  // bar 2 still in slot 2
    }
    // Slot walk: K = 6, bar 8 reads slots 8%6=2 (this call, 1.0), 7%6=1
    // (never), 6%6=0 (never), 5%6=5 (bar 5, 2.0), 4%6=4 (bar 4, 3.0). Bar 2
    // wrote slot 2 — overwritten by bar 8 — so the answer is 3.0, not 7.0.
    {
        ta::Highest sparse(5);
        { ta::BarContextScope scope(2, 0); sparse.compute(7.0); }
        { ta::BarContextScope scope(4, 0); sparse.compute(3.0); }
        { ta::BarContextScope scope(5, 0); sparse.compute(2.0); }
        { ta::BarContextScope scope(8, 0); CHECK(near(sparse.compute(1.0), 3.0)); }
    }
    // Origin: a feed whose first bar is Pine bar 7 warms up over ITS first
    // `length` bars (identical to the deque), not TV's bar_index rule alone.
    {
        ta::Highest hi(3);
        { ta::BarContextScope scope(7, 7); CHECK(is_na(hi.compute(1.0))); }
        { ta::BarContextScope scope(8, 7); CHECK(is_na(hi.compute(2.0))); }
        { ta::BarContextScope scope(9, 7); CHECK(near(hi.compute(0.5), 2.0)); }
    }
    // length <= 0 stays na forever (compat with the deque's empty window).
    {
        ta::Highest zero(0);
        ta::Lowest neg(-2);
        for (int b = 0; b < 4; ++b) {
            ta::BarContextScope scope(b, 0);
            CHECK(is_na(zero.compute(1.0)));
            CHECK(is_na(neg.compute(1.0)));
        }
    }
}

// --- The *Bars variants: bars back = k of the extremum slot ---

static void test_bars_variants_ring() {
    std::printf("test_bars_variants_ring\n");
    // Cadence 7, K=6: bar 3's 120 beats bar 10's 110 one slot behind.
    // Bar 31's peak 170 stays at offsets 1,2,3,4 on calls at 38..59;
    // bar 66's new peak 210 resets the offset to 0. Negation mirrors it.
    static const double kExpect[10] = {0, -1, 0, 0, 0, -1, -2, -3, -4, 0};
    ta::HighestBars hb(5);
    ta::LowestBars lb(5);
    int call = 0;
    for (int b = 0; b < 70; ++b) {
        ta::BarContextScope scope(b, 0);
        if (b % 7 != 3) continue;
        double v = hb.compute(synthetic_high(b));
        double w = lb.compute(-synthetic_high(b));
        if (call == 0) {
            CHECK(is_na(v));
            CHECK(is_na(w));
        } else {
            CHECK(near(v, kExpect[call]));
            CHECK(near(w, kExpect[call]));   // mirror: lowestbars(-x) == highestbars(x)
        }
        ++call;
    }
    CHECK(call == 10);

    // Fresh-window tie rule: the OLDEST equal extremum wins (as the deque scan
    // did), in both context and standalone mode.
    {
        ta::HighestBars t(4);
        ta::LowestBars u(4);
        double xs[4] = {5.0, 9.0, 9.0, 1.0};
        double ys[4] = {5.0, 1.0, 1.0, 9.0};
        double v = na<double>(), w = na<double>();
        for (int b = 0; b < 4; ++b) {
            ta::BarContextScope scope(b, 0);
            v = t.compute(xs[b]);
            w = u.compute(ys[b]);
        }
        CHECK(near(v, -2.0));
        CHECK(near(w, -2.0));
        ta::HighestBars t2(4);
        for (int b = 0; b < 4; ++b) v = t2.compute(xs[b]);
        CHECK(near(v, -2.0));
    }

    // na input: written as a positional member (it poisons the older ones)
    // and answers na on that bar; the offset then points at the real bar.
    // The deque skipped na without advancing, so {9, na, 7, 2} at length 3
    // answered -2 (the 9, actually 3 bars back) — TV's series is positional.
    {
        ta::HighestBars t(3);
        double v = na<double>();
        double xs[4] = {9.0, na<double>(), 7.0, 2.0};
        for (int b = 0; b < 4; ++b) {
            ta::BarContextScope scope(b, 0);
            v = t.compute(xs[b]);
            if (b == 1) CHECK(is_na(v));
        }
        CHECK(near(v, -1.0));   // window {na, 7, 2} -> 7, one bar back
    }
}

// --- Every-bar callers: byte-identical to the deque, in every mode ---

// mode 0: no context installed (standalone / unit-test cadence)
// mode 1: chart context, bars 0..n-1, origin 0
// mode 2: chart context after a bar_index_offset: bars 1000.., origin 1000
// Every third bar also issues a recompute() with a different value (bar
// magnifier / lookahead partial ticks) before moving on.
template <class Ring, class Ref>
static void every_bar_identity(int length, int mode, int na_every, unsigned seed) {
    const int n = 257;
    std::vector<double> xs = make_series(n, seed, na_every);
    std::vector<double> ys = make_series(n, seed ^ 0x9e3779b9u, na_every);
    Ring ring(length);
    Ref ref(length);
    for (int b = 0; b < n; ++b) {
        long long bar = mode == 2 ? 1000 + b : b;
        long long origin = mode == 2 ? 1000 : 0;
        if (mode == 0) {
            CHECK(same(ring.compute(xs[b]), ref.compute(xs[b])));
            if (b % 3 == 2) CHECK(same(ring.recompute(ys[b]), ref.recompute(ys[b])));
        } else {
            ta::BarContextScope scope(bar, origin);
            CHECK(same(ring.compute(xs[b]), ref.compute(xs[b])));
            if (b % 3 == 2) CHECK(same(ring.recompute(ys[b]), ref.recompute(ys[b])));
        }
    }
}

static void test_every_bar_identity() {
    std::printf("test_every_bar_identity\n");
    for (int length : {1, 2, 3, 5, 8, 13, 50}) {
        for (int mode = 0; mode < 3; ++mode) {
            // Highest / Lowest: positional deque incl. na gaps (under the
            // written-na poison) -> identical with gaps planted every 7th bar.
            every_bar_identity<ta::Highest, RefHighest>(length, mode, 7, 17u + length);
            every_bar_identity<ta::Lowest, RefLowest>(length, mode, 7, 29u + length);
            every_bar_identity<ta::Highest, RefHighest>(length, mode, 0, 3u + length);
            every_bar_identity<ta::Lowest, RefLowest>(length, mode, 0, 5u + length);
            // *Bars: identical on gap-free series (the deque's na rule was
            // non-positional; see test_bars_variants_ring).
            every_bar_identity<ta::HighestBars, RefHighestBars>(length, mode, 0, 7u + length);
            every_bar_identity<ta::LowestBars, RefLowestBars>(length, mode, 0, 11u + length);
        }
    }
    // recompute() before the first compute() behaves as the first bar (the
    // pristine rule the recompute suite pins), with and without a context.
    {
        ta::Highest a(3);
        RefHighest r(3);
        CHECK(same(a.recompute(2.0), r.recompute(2.0)));
        CHECK(same(a.compute(5.0), r.compute(5.0)));
        CHECK(same(a.compute(1.0), r.compute(1.0)));
        ta::HighestBars hb(2);
        CHECK(is_na(hb.recompute(1.0)));
        ta::BarContextScope scope(0, 0);
        ta::Lowest c(2);
        CHECK(is_na(c.recompute(4.0)));
    }
}

// --- request.security context: its own index, nested inside the chart's ---

// Models the engine's dispatch: the chart scope wraps on_bar with the Pine
// bar_index; each security evaluation is dispatched under the requested
// context's evaluated-bar index (complete: eval_complete_count - 1, partial:
// eval_complete_count). An HTF of ratio 4 with lookahead partial ticks: the
// every-bar HTF caller must equal the deque fed one value per HTF bar, the
// partial recomputes must not advance it, and the chart's own ring must be
// unaffected by the nested scope.
static void test_htf_context_sanity() {
    std::printf("test_htf_context_sanity\n");
    const int ratio = 4;
    const int n = 200;
    std::vector<double> chart = make_series(n, 99u, 0);
    ta::Highest chart_hi(5);
    RefHighest chart_ref(5);
    ta::Highest htf_hi(3);
    RefHighest htf_ref(3);
    ta::Highest htf_cond(3);   // conditional HTF caller: executes on even HTF bars only
    long long eval_complete = 0;
    double bucket_high = na<double>();
    int sub = 0;
    for (int b = 0; b < n; ++b) {
        ta::BarContextScope chart_scope(b, 0);
        // --- security feed for this chart bar (engine: before on_bar) ---
        bucket_high = is_na(bucket_high) ? chart[b] : std::fmax(bucket_high, chart[b]);
        ++sub;
        const bool complete = (sub == ratio);
        if (complete) ++eval_complete;
        const long long sec_bar = complete ? eval_complete - 1 : eval_complete;
        {
            ta::BarContextScope sec_scope(sec_bar, 0);
            CHECK(ta::bar_context().bar_index == sec_bar);
            // codegen: compute on the bucket's first tick, recompute after.
            if (sub == 1) {
                htf_hi.compute(bucket_high);
                htf_ref.compute(bucket_high);
            } else {
                double v = htf_hi.recompute(bucket_high);
                CHECK(same(v, htf_ref.recompute(bucket_high)));
            }
            if (complete && (sec_bar % 2 == 0)) {
                double v = htf_cond.recompute(bucket_high);
                // Conditional HTF caller, K = 4: HTF bar 2 reads slots 2,1,0
                // -> bars 2 and 0 written -> max of the two even buckets so
                // far; never na past HTF bar 1.
                if (sec_bar >= 2) CHECK(!is_na(v));
                if (sec_bar == 0) CHECK(is_na(v));
            }
        }
        CHECK(ta::bar_context().installed && ta::bar_context().bar_index == b);
        if (complete) {
            sub = 0;
            bucket_high = na<double>();
        }
        // --- chart on_bar ---
        CHECK(same(chart_hi.compute(chart[b]), chart_ref.compute(chart[b])));
    }
    CHECK(!ta::bar_context().installed);
}


// --- Cached extremum survives aliasing until its implied bar expires ---
//
// Bar 3 leaves 90 in slot 3. Bar 42's live minimum is 110. At bars 48..51,
// slot 3 aliases into the window but the cache is younger than length 10.
// At bar 52 it expires, so the rescan finally reads the stale 90.
static void test_cached_extremum_keeps_over_aliased_slot_until_expiry() {
    std::printf("test_cached_extremum_keeps_over_aliased_slot_until_expiry\n");
    ta::Lowest lo(10);
    auto low_at = [](int b) {
        if (b == 3) return 90.0;
        if (b < 4) return 95.0;
        if (b == 42) return 110.0;
        if (b >= 37 && b <= 46) return 120.0;
        return 130.0;                       // bars 48..52
    };
    auto executes = [](int b) {
        const int m = b % 49;
        return m < 4 || (m >= 37 && m < 47) || m == 48 || (b >= 49 && b <= 52);
    };
    for (int b = 0; b <= 52; ++b) {
        ta::BarContextScope scope(b, 0);
        if (!executes(b)) continue;
        const double v = lo.compute(low_at(b));
        if (b < 9) CHECK(is_na(v));                       // bar_index < length - 1
        if (b == 46) CHECK(near(v, 110.0));                // fresh window minimum, bar 42
        if (b >= 48 && b <= 51) CHECK(near(v, 110.0));     // cache (110.0, 42) younger than 10 bars: slot 3 unread
        if (b == 52) CHECK(near(v, 90.0));                // aged out: rescan reads the aliased slot
    }
}

// ETH #88 geometry (ta.lowest(low, 10) inside `if strategy.position_size > 0`):
// a 17-bar run whose 7th bar leaves 2641.73 in the slot that aliases k=6
// behind the next run's first bar, then a 50-bar gap. TV answers the run-start
// bar's own low 2648.62 because it beats the expired cache (2650.47) — the
// new-extremum test comes before the rescan. The control run-start (r17g50 @
// bar 67: own low above the cache) rescans and reads the aliased slot.
static void run_eth88_geometry(double run_start_low, double expected) {
    ta::Lowest lo(10);
    for (int b = 0; b <= 87; ++b) {
        ta::BarContextScope scope(b, 0);
        const bool in_run = b >= 20 && b <= 36;
        if (!in_run && b != 87) continue;
        double src;
        if (b == 26) src = 2641.73;                       // the 7th bar of the run: slot 26 % 11 = 4
        else if (in_run) src = 2650.47 + (b % 3) * 0.5;   // minimum 2650.47 at bars 21, 24, ...
        else src = run_start_low;                          // bar 87: 87 - 6 = 81, 81 % 11 = 4 -> the aliased slot
        const double v = lo.compute(src);
        if (b == 35) CHECK(near(v, 2641.73));             // cache (2641.73, 26) still 9 bars young
        if (b == 36) CHECK(near(v, 2650.47));             // aged out at 10 bars: rescan of 27..36
        if (b == 87) CHECK(near(v, expected));
    }
}

static void test_new_extremum_precedes_expiry_rescan() {
    std::printf("test_new_extremum_precedes_expiry_rescan\n");
    run_eth88_geometry(2648.62, 2648.62);   // ETH #88: below the expired cache -> own low, no rescan
    run_eth88_geometry(2652.00, 2641.73);   // r17g50 @67: above the cache -> rescan reads the aliased slot
    CHECK(!ta::bar_context().installed);
}


// --- Never-written slots read as 0 (pinned 2026-09-04, BINANCE:BTCUSDT 1D,
// `if bar_index % 13 == 5`, K = 11): ta.lowest(low, 10) answers na on the
// first call (bar 5 < 9), 0 on calls 2..10 while a never-written slot sits
// in the scanned window, then the stale positive minimum from call 11 on
// once every residue has been written (TV: 77.3k, 84.5k); ta.highest(-close,
// 10) mirrors it (0, then negative); ta.highest(high, 10) is unaffected. ----
static void test_never_written_slots_read_as_zero() {
    std::printf("test_never_written_slots_read_as_zero\n");
    ta::Lowest lo(10);
    ta::Highest hi(10);
    ta::Highest hneg(10);
    int call = 0;
    for (int b = 0; b <= 200; ++b) {
        ta::BarContextScope scope(b, 0);
        if (b % 13 != 5) continue;
        ++call;
        const double price = 100000.0 + b;         // rising, so every call is a fresh high, never a new low
        const double l = lo.compute(price);
        const double h = hi.compute(price);
        const double n = hneg.compute(-price);
        if (call == 1) { CHECK(is_na(l)); CHECK(is_na(h)); CHECK(is_na(n)); continue; }
        CHECK(near(h, price));                     // a rising series: the current high wins
        if (call <= 10) {                          // residues 5,7,9,0,2,4,6,8,10,1 written so far: a slot in the window is still unwritten
            CHECK(near(l, 0.0));
            CHECK(near(n, 0.0));
        } else {                                   // call 11 writes residue 3: every slot written, stale values return
            CHECK(!is_na(l) && l > 0.0);
            CHECK(!is_na(n) && n < 0.0);
        }
    }
    CHECK(call == 16);
}



// --- A written na poisons older members and restarts the next valid call ---
//
// Synthetic four-bar blocks: gap, base+6, base, base+9, base=200+10*(b/4).
// After a gap, only base+6 is live; two calls later the extrema are base and
// base+9. Warmup bars 1/3 are na. A pre-gap low must not survive the gap.
static double synthetic_low(int b) {
    static const double kWithinBlock[4] = {0.0, 6.0, 0.0, 9.0};
    return 200.0 + (b / 4) * 10.0 + kWithinBlock[b % 4];
}
// Odd-bar signals: qty = 1 + 100 * extremum, na -> 7.
// In block g >= 1, V is 200+10g+6 then 200+10g; H is 200+10g+6 then
// 200+10g+9. Thus every listed integer follows directly from the block rule.
static const int kSyntheticV[20] = {
    7, 7, 21601, 21001, 22601, 22001, 23601, 23001, 24601, 24001,
    25601, 25001, 26601, 26001, 27601, 27001, 28601, 28001, 29601, 29001};
static const int kSyntheticH[20] = {
    7, 7, 21601, 21901, 22601, 22901, 23601, 23901, 24601, 24901,
    25601, 25901, 26601, 26901, 27601, 27901, 28601, 28901, 29601, 29901};

static bool matches_qty(double v, int qty) {
    if (qty == 7) return is_na(v);
    if (is_na(v)) return false;
    return std::fabs(v - (qty - 1) / 100.0) <= 0.0051;   // round(v * 100) resolution
}

static void test_everybar_na_gap_poisons_the_window() {
    std::printf("test_everybar_na_gap_poisons_the_window\n");
    for (int mode = 0; mode < 2; ++mode) {          // 0: standalone cadence, 1: chart context
        ta::Lowest lo(5);
        ta::Highest hi(5);
        int entry = 0;
        for (int b = 0; b < 42; ++b) {
            const double x = (b % 4 == 0) ? na<double>() : synthetic_low(b);
            double v, h;
            if (mode == 0) {
                v = lo.compute(x);
                h = hi.compute(x);
            } else {
                ta::BarContextScope scope(b, 0);
                v = lo.compute(x);
                h = hi.compute(x);
            }
            if (b % 4 == 0) { CHECK(is_na(v)); CHECK(is_na(h)); }          // the na bar answers na
            if (b % 2 == 1 && b <= 39) {   // bar 41's signal has no next bar to fill on
                CHECK(entry < 20);
                if (entry < 20) {
                    CHECK(matches_qty(v, kSyntheticV[entry]));
                    CHECK(matches_qty(h, kSyntheticH[entry]));
                }
                ++entry;
            }
            if (b >= 4 && b % 4 == 1) CHECK(same(v, h));   // the bar after a gap: a single live member
            if (b == 9) { CHECK(near(v, 226.0)); CHECK(near(h, 226.0)); } // 200+20+6; pre-gap bar 6's 210 is dead
            if (b == 11) { CHECK(near(v, 220.0)); CHECK(near(h, 229.0)); } // min/max of {226,220,229}
        }
        CHECK(entry == 20);
    }
    CHECK(!ta::bar_context().installed);
}

// Synthetic cadence 13, length 10: call i is at 5+13*i, so K=11
// visits residues 5,7,9,0,2,4,6,8,10,1,3,5,7. Positive sources follow
// 200+20*i: unwritten zeros win through i=9; at i=10..12 the oldest
// available positive members are calls 0,1,2, giving 200,220,240.
// The sign-mixed source below is designed to keep negative minima ahead
// of unwritten zeros and to expose the stale slot containing -20.
static int synthetic_call_bar(int i) { return 5 + 13 * i; }
static double synthetic_positive_call(int i) { return 200.0 + 20.0 * i; }
static const double kSyntheticLowestLow[13] = {
    NAN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 200, 220, 240};
static const double kSyntheticOsc[13] = {
    6, -8, -12, -20, -4, 10, 14, -6, -16, -2, -10, -18, -24};
// At i=8 the -20 slot is excluded: min=-16. At i=9 it aliases back into
// the window: min=-20 until the fresh -24 at i=12 wins before the rescan.
static const double kSyntheticLowestOsc[13] = {
    NAN, -8, -12, -20, -20, -20, -20, -20, -16, -20, -20, -20, -24};

static void test_cadence13_zero_fill_control_and_computed_source() {
    std::printf("test_cadence13_zero_fill_control_and_computed_source\n");
    ta::Lowest lo(10);
    ta::Lowest lo_osc(10);
    for (int i = 0; i < 13; ++i) {
        ta::BarContextScope scope(synthetic_call_bar(i), 0);
        const double v = lo.compute(synthetic_positive_call(i));
        const double w = lo_osc.compute(kSyntheticOsc[i]);
        if (i == 0) { CHECK(is_na(v)); CHECK(is_na(w)); continue; }
        CHECK(!is_na(v) && near(v, kSyntheticLowestLow[i], 0.051));
        CHECK(!is_na(w) && near(w, kSyntheticLowestOsc[i], 0.006));
        if (i >= 1 && i <= 9) CHECK(near(v, 0.0));   // a never-written slot in the window reads 0
        if (i >= 1) CHECK(w < 0.0);                  // the 0 never beats a negative member
    }
}

// Synthetic leading-na sources at the same cadence. Positive sites use
// 400+100*s+20*i after 1,2,5 leading na writes, respectively. Signed sites
// use the hand-designed values below after 2 and 4 leading na writes.
// The na at residue 5 is read one slot behind call i=6, clearing older
// members. Calls i=6,7 therefore answer their own source; subsequent
// rescans can resurrect only slots NEWER than the last encountered na.
static double synthetic_na_first_source(int s, int i) {
    static const int kLeadingNa[5] = {1, 2, 5, 2, 4};
    static const double kSigned[2][13] = {
        {NAN, NAN, -24, -14, -2, -6, 12, -4, -8, 4, -32, -20, -48},
        {NAN, NAN, NAN, NAN, -16, -6, 8, -4, -10, 2, -36, -40, -72},
    };
    if (i < kLeadingNa[s]) return na<double>();
    return s < 3 ? 400.0 + 100.0 * s + 20.0 * i : kSigned[s - 3][i];
}
// Positive sites: zeros win until poison removes them; the stale first
// valid write wins at i=8..11 (site A/B), or i=11..12 (site E).
// Site A's first write is overwritten at i=12, so its second (440) wins.
// Signed sites take minima of the live post-poison members: in F the
// aliased -24 returns at i=9, then -32 wins; in G i=9 restarts on +2.
static const double kNaFirstTv[5][13] = {
    {NAN, 420, 0, 0, 0, 0, 520, 540, 420, 420, 420, 420, 440},
    {NAN, NAN, 540, 0, 0, 0, 620, 640, 540, 540, 540, 540, 540},
    {NAN, NAN, NAN, NAN, NAN, 700, 720, 740, 760, 780, 800, 700, 700},
    {NAN, NAN, -24, -24, -24, -24, 12, -4, -8, -24, -32, -32, -48},
    {NAN, NAN, NAN, NAN, -16, -16, 8, -4, -10, 2, -36, -40, -72},
};
static const double kNaFirstTol[5] = {0.051, 0.051, 0.051, 0.0051, 0.0051};   // retained 0.1 / 0.01 resolution

static void test_leading_na_site_poison_pin() {
    std::printf("test_leading_na_site_poison_pin\n");
    for (int s = 0; s < 5; ++s) {
        ta::Lowest lo(10);
        ta::LowestBars lb(10);
        for (int i = 0; i < 13; ++i) {
            ta::BarContextScope scope(synthetic_call_bar(i), 0);
            const double v = lo.compute(synthetic_na_first_source(s, i));
            const double k = lb.compute(synthetic_na_first_source(s, i));
            if (is_na(kNaFirstTv[s][i])) {
                CHECK(is_na(v));
                CHECK(is_na(k));
            } else {
                CHECK(!is_na(v) && near(v, kNaFirstTv[s][i], kNaFirstTol[s]));
                CHECK(!is_na(k));
            }
            if (s == 0) {
                // Site A, first valid source at call 1: the bars-since of the
                // extremum among the non-poisoned slots.
                if (i == 1) CHECK(near(k, 0.0));     // bar 18: restart on src (slot 6 behind it never read)
                if (i == 2) CHECK(near(k, -3.0));    // bar 31: never-written slot 6 = bar 28, the oldest 0 past bar 27's na
                if (i == 6) CHECK(near(k, 0.0));     // bar 83: the na slot at k = 1, src alone
                if (i == 8) CHECK(near(k, -3.0));    // bar 109: 420 lives in slot 7 = bar 106
                if (i == 12) CHECK(near(k, -9.0));   // bar 161: 440 in slot 9 = bar 152
            }
        }
    }
    // Site A, first three calls, spelled out: bar 5 writes na (cache (na, 5)),
    // bar 18 restarts on 420 (a rescan would have read the never-written
    // slot 6 = bar 17 as 0 -- the poisoned cache restarts on 420), bar 31 rescans: slots 0..4
    // (bars 22..26) never written -> 0, bar 27 = slot 5 -> na, poison; bar
    // 28 = slot 6 never written -> 0 restarts; 420 (bar 29) and 440 (bar
    // 31) do not beat it -> 0.
    {
        ta::Lowest lo(10);
        { ta::BarContextScope scope(5, 0);  CHECK(is_na(lo.compute(na<double>()))); }
        { ta::BarContextScope scope(18, 0); CHECK(near(lo.compute(420.0), 420.0)); }
        { ta::BarContextScope scope(31, 0); CHECK(near(lo.compute(440.0), 0.0)); }
    }
}

// Designed two-ring sparse call schedule, length 11 (K=12).
// Sixteen initial na writes poison the older slots. All later values are
// invented integers, with distinct positive, stale and zero-winning cases.
// The call at bar 240 excludes the na at residue 10 but reads residue 11
// (written at 227) as bar 239: stale +7 beats the fresh +2. At bar 273
// the two unwritten zeros beat the fresh -4, after all poison is excluded.
// The call addresses deliberately preserve those modulo-12 alignments.
struct MlCall { int bar; bool low_ring; double osc; double tv; };
static const MlCall kMlCalls[32] = {
    {18, false, NAN, NAN},      {25, true, NAN, NAN},       {28, false, NAN, NAN},
    {35, true, NAN, NAN},       {41, false, NAN, NAN},      {45, true, NAN, NAN},
    {50, false, NAN, NAN},      {52, true, NAN, NAN},       {57, false, NAN, NAN},
    {67, true, NAN, NAN},       {70, false, NAN, NAN},      {74, true, NAN, NAN},
    {80, true, NAN, NAN},       {84, false, NAN, NAN},      {89, true, NAN, NAN},
    {96, false, NAN, NAN},
    {104, true, 6, 6},    {147, false, 8, 8},    {153, true, 4, 4},
    {165, false, 5, 5},   {168, true, 3, 3},     {177, false, 2, 2},
    {195, false, 20, 20}, {198, true, 16, 16},   {218, false, 6, 6},
    {220, true, 10, 10},  {227, false, 7, 7},    {231, true, 5, 5},
    {240, false, 2, 7},   {241, true, 1, 1},     {255, true, -8, -8},
    {273, false, -4, 0},
};

static void test_market_logic_replica_pin() {
    std::printf("test_market_logic_replica_pin\n");
    ta::Lowest lo(11);
    ta::Highest hi(11);
    int ok = 0;
    for (const MlCall& c : kMlCalls) {
        ta::BarContextScope scope(c.bar, 0);
        const double v = c.low_ring ? lo.compute(c.osc) : hi.compute(c.osc);
        const bool hit = is_na(c.tv) ? is_na(v) : (!is_na(v) && near(v, c.tv, 0.0007));
        CHECK(hit);
        ok += hit;
        if (c.bar == 240) CHECK(!is_na(v) && near(v, 7.0, 1e-6));   // residue 11 aliases bar 227's +7 as bar 239
        if (c.bar == 273) CHECK(!is_na(v) && near(v, 0.0));            // a never-written slot beats every negative member
    }
    CHECK(ok == 32);
}

// The *Bars variants under poison (derived from the rule; no tape): the
// offset is the bars-since of the extremum among the non-poisoned members.
static void test_bars_variants_under_poison() {
    std::printf("test_bars_variants_under_poison\n");
    // Every bar, length 5: {9, 8, na, 7, 2, 1} -> bar 5's live members are
    // bars 3..5. highest = 7 at bar 3 (-2), lowest = 1 at bar 5 (0). The old
    // "skip the gap" reading answered 8 at bar 1 (-4) for the highest.
    {
        ta::HighestBars hb(5);
        ta::LowestBars lb(5);
        const double xs[6] = {9.0, 8.0, na<double>(), 7.0, 2.0, 1.0};
        double h = na<double>(), l = na<double>();
        for (int b = 0; b < 6; ++b) {
            ta::BarContextScope scope(b, 0);
            h = hb.compute(xs[b]);
            l = lb.compute(xs[b]);
            if (b == 2) { CHECK(is_na(h)); CHECK(is_na(l)); }
        }
        CHECK(near(h, -2.0));
        CHECK(near(l, 0.0));
    }
    // Same series through a Range (the composites share the ring): bar 5's
    // range is 7 - 1 = 6 over the live members, not 9 - 1 = 8.
    {
        ta::Range r(5);
        const double xs[6] = {9.0, 8.0, na<double>(), 7.0, 2.0, 1.0};
        double v = na<double>();
        for (int b = 0; b < 6; ++b) {
            ta::BarContextScope scope(b, 0);
            v = r.compute(xs[b]);
            if (b == 2) CHECK(is_na(v));
        }
        CHECK(near(v, 6.0));
    }
    // A leading-na series (the finding-331 stochRSI shape) is unchanged by
    // the poison: the first live bar is a single-member window.
    {
        ta::Stoch st(3);
        CHECK(is_na(st.compute(na<double>(), na<double>(), na<double>())));
        CHECK(is_na(st.compute(na<double>(), na<double>(), na<double>())));
        CHECK(is_na(st.compute(na<double>(), na<double>(), na<double>())));
        CHECK(is_na(st.compute(50.0, 50.0, 50.0)));            // hi == lo -> division by zero -> na
        CHECK(near(st.compute(60.0, 60.0, 60.0), 100.0));      // window {50, 60}: (60 - 50) / 10
    }
}

// An na source poisons the cache. Every valid call after it must restart
// on its own source, even for a rising series: retaining the old cache would
// rescan unwritten/stale slots on expiry. Cadence and sources are invented.
static void test_na_source_poisons_the_cache() {
    std::printf("test_na_source_poisons_the_cache\n");
    for (int falling = 0; falling < 2; ++falling) {
        ta::Lowest lo(10);
        int checked = 0;
        for (int b = 0; b <= 200; ++b) {
            ta::BarContextScope scope(b, 0);
            const bool valid_call = b % 13 == 5;
            const bool na_call = b >= 100 && b % 13 == 6;
            if (!valid_call && !na_call) continue;
            if (na_call) {
                CHECK(is_na(lo.compute(na<double>())));
                continue;
            }
            const double src = falling ? 3000.0 - b : 3000.0 + b;
            const double v = lo.compute(src);
            if (b >= 122) { CHECK(near(v, src)); ++checked; }   // the na call 12 bars earlier poisoned the cache
        }
        CHECK(checked == 7);
    }
    CHECK(!ta::bar_context().installed);
}

int main() {
    test_cadence7_pin();
    test_cadence9_pin();
    test_cadence_pin_lowest_mirror();
    test_na_until_bar_length_minus_1();
    test_bars_variants_ring();
    test_every_bar_identity();
    test_htf_context_sanity();
    test_cached_extremum_keeps_over_aliased_slot_until_expiry();
    test_new_extremum_precedes_expiry_rescan();
    test_never_written_slots_read_as_zero();
    test_na_source_poisons_the_cache();
    test_everybar_na_gap_poisons_the_window();
    test_cadence13_zero_fill_control_and_computed_source();
    test_leading_na_site_poison_pin();
    test_market_logic_replica_pin();
    test_bars_variants_under_poison();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
