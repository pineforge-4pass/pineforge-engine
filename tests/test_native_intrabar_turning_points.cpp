// R5 lane MAG-INTRABAR: a retained lower bar is walked through its four
// turning points, a degenerate leg's repeated one included.
//
// deliver_intrabar_script walks each retained lower-timeframe bar as its
// open (a discrete point) and three continuous segments, one per turning
// point, and offers the margin model a check at every sample after the
// script bar's first (NativeMarginCheckKind::IntrabarSample). A bar whose
// leg has zero length -- an open at its own low -- is declined by the direct
// four-sample path, and the general ENDPOINTS sampler it fell to fills the
// missing point with a uniform one: a sample the bar never printed there.
// TradingView's magnified broker walks an intrabar as its four ticks
// whatever their values (tests/fixtures/intrabar_margin/pm2-i3-pooc-mag-
// 0402-2000: its first call is booked at an intrabar that opens at its low).
//
// One long, 20 units at 100, maintenance 0.25: equity 20p - 1000 falls
// under the requirement 5p below 66.67. Bar 2's second sub-bar opens at its
// own low, 60, and rises to 90:
//   1. the samples: that sub-bar's four marks are 60, 60, 90, 85 -- its
//      open, its (repeated) low, its high and its close; before, the second
//      was the general sampler's uniform fill, 77.5, on the way up;
//   2. the call: the check at the open arms the liquidation at 60 and the
//      zero-length segment into the low reaches it, so it is booked there;
//      before, the next segment rose away from it, the check at the fill
//      point found the book solvent again and withdrew it, and the bar ended
//      with no call;
//   3. a bar with four distinct turning times keeps its direct samples;
//   4. a bar that traded nothing at one price is one print, walked once:
//      bar 3's first sub-bar (90, volume 0) gives one mark where it gave
//      four, and a one-price sub-bar that traded keeps its four.
//
// Source-free: runs in the kernel-only profile too.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;
#define CHECK(expr) do {                                                       \
    ++checks;                                                                  \
    if (!(expr)) { ++failures; std::printf("FAIL %d %s\n", __LINE__, #expr); } \
} while (0)

bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

struct Mark {
    NativeMarginCheckKind kind;
    int bar;
    double mark;
};

struct Probe final : NativeStrategyHost {
    int bars = 0;
    mutable std::vector<Mark> marks;
    std::vector<no::MarginCallEvent> calls;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bars++ == 0) submit({no::Transact{20.0}, "long", ""});
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& p) const override {
        marks.push_back({p.kind, p.cursor.point.interval_index, p.mark});
        return true;
    }
    void on_native_margin_call(const no::MarginCallEvent& e) override { calls.push_back(e); }
};

const int64_t T0 = 1791190800000LL;   // Mon 5 Oct 2026 09:00 UTC
const int64_t M15 = 15 * 60000LL;
const int64_t M5 = 5 * 60000LL;

std::vector<Bar> input() {
    return {{100.0, 100.5, 99.5, 100.0, 10, T0},
            {100.0, 101.0, 99.0, 100.0, 10, T0 + M15},      // the long fills at 100
            {100.0, 100.0, 60.0, 90.0, 10, T0 + 2 * M15},   // gaps to 60 inside, recovers
            {90.0, 91.0, 89.0, 90.0, 10, T0 + 3 * M15}};
}

std::vector<Bar> finer() {
    std::vector<Bar> out;
    for (const Bar& b : input()) {
        if (b.low == 60.0) {
            out.push_back({100.0, 100.0, 95.0, 96.0, 3, b.timestamp});
            out.push_back({60.0, 90.0, 60.0, 85.0, 4, b.timestamp + M5});   // opens at its low
            out.push_back({85.0, 92.0, 84.0, 90.0, 3, b.timestamp + 2 * M5});
        } else if (b.open == 90.0) {
            out.push_back({90.0, 90.0, 90.0, 90.0, 0, b.timestamp});        // one print
            out.push_back({90.0, 91.0, 89.0, 90.0, 3, b.timestamp + M5});
            out.push_back({90.0, 90.0, 90.0, 90.0, 4, b.timestamp + 2 * M5});   // one price, traded
        } else {
            out.push_back({b.open, b.high, b.low, b.close, 3, b.timestamp});
            out.push_back({b.close, b.high, b.low, b.close, 4, b.timestamp + M5});
            out.push_back({b.close, b.high, b.low, b.close, 3, b.timestamp + 2 * M5});
        }
    }
    return out;
}

NativeRunSpec spec() {
    NativeRunSpec s;
    s.identity = {"intrabar-turning-points", 1};
    s.input_tf = "15";
    s.script_tf = "15";
    s.ticker = "P";
    s.tickerid = "PROBE:P";
    s.type = "cfd";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    NativeMarginModel m;
    m.initial_long = 0.0;
    m.maintenance_long = 0.25;
    m.initial_short = 0.0;
    m.maintenance_short = 0.25;
    m.sizing = NativeLiquidationSizing::RestoreMinimum;
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;
    s.margin = m;
    IntrabarPath::lower_tf lower;
    lower.bars = finer();
    lower.tf = "5";
    s.intrabar.value = lower;
    return s;
}

void run(Probe& h) {
    const auto setup = h.configure_native(spec());
    CHECK(setup.status == NativeSetupStatus::Applied);
    if (setup.status != NativeSetupStatus::Applied) return;
    const auto bars = input();
    h.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(h.last_error().empty());
}

// Bar 2's marks, in order: 12 samples of its three sub-bars.
std::vector<double> bar_marks(const Probe& h, int bar) {
    std::vector<double> out;
    for (const auto& m : h.marks)
        if (m.bar == bar && (m.kind == NativeMarginCheckKind::BarOpen
                             || m.kind == NativeMarginCheckKind::IntrabarSample))
            out.push_back(m.mark);
    return out;
}

void print(const Probe& h) {
    std::printf("bar 2 marks:");
    for (double m : bar_marks(h, 2)) std::printf(" %.2f", m);
    std::printf("\ncalls %zu", h.calls.size());
    for (const auto& c : h.calls)
        std::printf("  [bar %d units %.6g at %.4f]", c.cursor.point.interval_index, c.units, c.mark);
    std::printf("  position %.6g\n", h.physical_position().signed_units);
}

// 1. The samples of the bar that opens at its low.
void degenerate_leg_samples() {
    Probe h;
    run(h);
    print(h);
    const auto marks = bar_marks(h, 2);
    CHECK(marks.size() >= 8);
    if (marks.size() >= 8) {
        // First sub-bar (100 95 96 after a flat leg): the direct samples.
        // Second: 60 (open), 60 (the repeated low), 90 (high), 85 (close).
        CHECK(marks[4] == 60.0);
        CHECK(marks[5] == 60.0);
        CHECK(marks[6] == 90.0);
        CHECK(marks[7] == 85.0);
    }
}

// 2. The call is booked at the open that is also the low.
void call_at_the_low_open() {
    Probe h;
    run(h);
    CHECK(h.calls.size() == 1);
    if (!h.calls.empty()) {
        CHECK(h.calls[0].cursor.point.interval_index == 2);
        CHECK(h.calls[0].mark == 60.0);
        CHECK(h.calls[0].units > 0.0);
    }
    CHECK(h.physical_position().signed_units < 20.0);
}

// 3. Four distinct turning times keep the direct samples.
void direct_samples_unchanged() {
    Probe h;
    run(h);
    const auto marks = bar_marks(h, 2);
    CHECK(marks.size() >= 12);
    if (marks.size() >= 12) {
        // Third sub-bar 85 92 84 90, open nearer its low: 85 84 92 90, the
        // direct path's own samples (path_at, within an ULP of the points).
        CHECK(marks[8] == 85.0);
        CHECK(near(marks[9], 84.0));
        CHECK(near(marks[10], 92.0));
        CHECK(marks[11] == 90.0);
    }
}

// 4. A one-price bar that traded nothing is one point.
void one_print_is_one_point() {
    Probe h;
    run(h);
    const auto marks = bar_marks(h, 3);
    std::printf("bar 3 marks:");
    for (double m : marks) std::printf(" %.2f", m);
    std::printf("\n");
    // 90 | 90 89 91 90 (open equidistant from both: low first) | 90 90 90 90.
    CHECK(marks.size() == 9);
    if (marks.size() == 9) {
        CHECK(marks[0] == 90.0);
        CHECK(marks[1] == 90.0);
        CHECK(near(marks[2], 89.0));
        CHECK(near(marks[3], 91.0));
        CHECK(marks[4] == 90.0);
        for (std::size_t k = 5; k < 9; ++k) CHECK(marks[k] == 90.0);
    }
}

}  // namespace

int main() {
    degenerate_leg_samples();
    call_at_the_low_open();
    direct_samples_unchanged();
    one_print_is_one_point();
    std::printf("test_native_intrabar_turning_points: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
