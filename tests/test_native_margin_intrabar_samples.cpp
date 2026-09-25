// R5 lane PAR-MARGIN, item 3: the margin model on an intrabar path.
//
// A run with an IntrabarPath has no whole-bar waypoint model, and the kernel
// documented that it "re-evaluates at each delivered sample" -- but it checked
// only the script bar's first sample (BarOpen) and the re-arm after fills, so
// a breach inside a magnified bar that recovered by the next open was never
// booked (lane H-MEASURE's acid finding A). NativeMarginCheckKind::
// IntrabarSample is that per-sample check: every delivered sample after the
// bar's first is offered to the host, measured at its own price, immediately
// before it is matched. TradingView's magnified broker checks the same way
// (tests/fixtures/intrabar_margin; the adapter half is
// test_adapter_margin_schedule_differential's "intrabar margin on tapes").
//
// One short on a maintenance-only side, the run fed as 15m bars and, for the
// intrabar runs, the same bars as three 5m sub-bars each. Bar 2 first crosses
// the liquidation level at its first sub-bar's high (106) and reaches 110 in
// its second:
//   1. the points: a lower_tf run matched as continuous segments offers
//      IntrabarSample at every sample but each bar's first (11 of 12 here),
//      each at that sample's own price, and BarOpen at the first; a run
//      without an intrabar path offers none, nor does a CalculationOnly model;
//   2. the mark check books the call AT the crossing sample (106), sized
//      there, where the whole-bar model books it at the bar's extreme (110);
//   3. a host that refuses IntrabarSample keeps the earlier behaviour: the
//      bar recovers by the next open and nothing is booked;
//   4. a one-price distribution path (synthesized) is not offered the point:
//      a request armed at a discrete point is matched only at a later one, so
//      it keeps its first sample and the re-arm after fills;
//   5. the C spelling is the kernel's value.
//
// Source-free: runs in the kernel-only profile too.
#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
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

static_assert(static_cast<int>(NativeMarginCheckKind::IntrabarSample)
                  == PF_NATIVE_MARGIN_CHECK_INTRABAR_SAMPLE,
              "the C spelling names the kernel's value");

struct Point {
    NativeMarginCheckKind kind;
    int bar;
    double mark;
    std::uint64_t ordinal;
};

struct Probe final : NativeStrategyHost {
    int bars = 0;
    bool refuse_samples = false;
    mutable std::vector<Point> points;
    std::vector<no::MarginCallEvent> calls;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bars++ == 0) submit({no::Transact{-174.0}, "short", ""});
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& p) const override {
        points.push_back({p.kind, p.cursor.point.interval_index, p.mark, p.cursor.point.ordinal});
        return !(refuse_samples && p.kind == NativeMarginCheckKind::IntrabarSample);
    }
    void on_native_margin_call(const no::MarginCallEvent& e) override { calls.push_back(e); }
    int count(NativeMarginCheckKind kind) const {
        int n = 0;
        for (const auto& p : points) n += p.kind == kind;
        return n;
    }
};

const int64_t T0 = 1791190800000LL;   // Mon 5 Oct 2026 09:00 UTC
const int64_t M15 = 15 * 60000LL;
const int64_t M5 = 5 * 60000LL;

std::vector<Bar> input() {
    return {{100.0, 100.2, 99.8, 100.0, 10, T0},
            {100.0, 100.2, 99.8, 100.0, 10, T0 + M15},        // the short fills at 100
            {100.0, 110.0, 99.9, 100.0, 10, T0 + 2 * M15},    // crosses at 106, then 110
            {100.0, 100.2, 99.8, 100.0, 10, T0 + 3 * M15},    // opens under the level again
            {100.0, 100.2, 99.8, 100.0, 10, T0 + 4 * M15}};
}

// Three 5m sub-bars per 15m bar, each O H L C with a high-first leg order.
std::vector<Bar> finer() {
    std::vector<Bar> out;
    for (const Bar& b : input()) {
        if (b.high == 110.0) {
            out.push_back({100.0, 106.0, 99.9, 105.0, 3, b.timestamp});
            out.push_back({105.0, 110.0, 104.0, 109.0, 4, b.timestamp + M5});
            out.push_back({109.0, 109.5, 100.0, 100.0, 3, b.timestamp + 2 * M5});
        } else {
            out.push_back({b.open, b.high, b.low, 100.1, 3, b.timestamp});
            out.push_back({100.1, 100.1, 100.0, 100.0, 4, b.timestamp + M5});
            out.push_back({100.0, 100.0, 100.0, b.close, 3, b.timestamp + 2 * M5});
        }
    }
    return out;
}

enum class Path { Chart, LowerTf, Synthesized };

NativeRunSpec spec(Path path, NativeLiquidationCheck check) {
    NativeRunSpec s;
    s.identity = {"margin-intrabar-samples", 1};
    s.input_tf = "15";
    s.script_tf = "15";
    s.ticker = "P";
    s.tickerid = "PROBE:P";
    s.type = "cfd";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.event_retention = NativeEventRetention::Full;
    NativeMarginModel m;
    m.initial_long = 0.5;
    m.initial_short = 0.0;            // maintenance-only short side
    m.maintenance_short = 0.5;
    m.sizing = NativeLiquidationSizing::RestoreMinimum;
    m.check = check;
    s.margin = m;
    if (path == Path::LowerTf) {
        IntrabarPath::lower_tf lower;
        lower.bars = finer();
        lower.tf = "5";
        s.intrabar.value = lower;
    } else if (path == Path::Synthesized) {
        IntrabarPath::synthesized sampled;
        sampled.samples = 8;
        sampled.distribution = MagnifierDistribution::ENDPOINTS;
        s.intrabar.value = sampled;
    }
    return s;
}

void run(Probe& h, Path path, NativeLiquidationCheck check) {
    const auto setup = h.configure_native(spec(path, check));
    CHECK(setup.status == NativeSetupStatus::Applied);
    if (setup.status != NativeSetupStatus::Applied) return;
    const auto bars = input();
    h.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(h.last_error().empty());
}

void print(const char* label, const Probe& h) {
    std::printf("%-30s BarOpen %d AfterApplied %d IntrabarSample %d  calls %zu", label,
                h.count(NativeMarginCheckKind::BarOpen),
                h.count(NativeMarginCheckKind::AfterApplied),
                h.count(NativeMarginCheckKind::IntrabarSample), h.calls.size());
    for (const auto& c : h.calls)
        std::printf("  [bar %d units %.6g at %.4f]", c.cursor.point.interval_index, c.units, c.mark);
    std::printf("  position %.6g\n", h.physical_position().signed_units);
}

// 1. The points.
void offered_points() {
    Probe lower;
    run(lower, Path::LowerTf, NativeLiquidationCheck::PathAdverseExtremeMark);
    print("lower_tf, mark check", lower);
    CHECK(lower.count(NativeMarginCheckKind::BarOpen) == 5);
    // Three retained sub-bars of four turning points each: every sample but
    // each bar's first, at least on the bars no liquidation re-armed.
    CHECK(lower.count(NativeMarginCheckKind::IntrabarSample) >= 5 * 11 - 1);
    // Each point follows the last, and each sample is measured at its own
    // price: inside its bar's range, bar 2's crossing at 106 before its 110.
    const auto bars = input();
    std::uint64_t last = 0;
    bool increasing = true;
    bool in_range = true;
    int at_106 = -1;
    int at_110 = -1;
    for (std::size_t i = 0; i < lower.points.size(); ++i) {
        const auto& p = lower.points[i];
        if (p.ordinal < last) increasing = false;
        last = p.ordinal;
        if (p.kind != NativeMarginCheckKind::IntrabarSample) continue;
        const Bar& b = bars[static_cast<std::size_t>(p.bar)];
        if (p.mark < b.low || p.mark > b.high) in_range = false;
        if (p.bar == 2 && p.mark == 106.0 && at_106 < 0) at_106 = static_cast<int>(i);
        if (p.bar == 2 && p.mark == 110.0 && at_110 < 0) at_110 = static_cast<int>(i);
    }
    CHECK(increasing);
    CHECK(in_range);
    CHECK(at_106 >= 0 && at_110 > at_106);

    Probe chart;
    run(chart, Path::Chart, NativeLiquidationCheck::PathAdverseExtremeMark);
    print("chart, mark check", chart);
    CHECK(chart.count(NativeMarginCheckKind::IntrabarSample) == 0);

    Probe calc;
    run(calc, Path::LowerTf, NativeLiquidationCheck::CalculationOnly);
    print("lower_tf, CalculationOnly", calc);
    CHECK(calc.count(NativeMarginCheckKind::IntrabarSample) == 0);
    CHECK(calc.count(NativeMarginCheckKind::BarOpen) == 0);
}

// 2. Where the call lands.
void call_at_the_crossing_sample() {
    Probe chart;
    run(chart, Path::Chart, NativeLiquidationCheck::PathAdverseExtremeMark);
    Probe lower;
    run(lower, Path::LowerTf, NativeLiquidationCheck::PathAdverseExtremeMark);
    // The whole-bar model: one call at bar 2's extreme.
    CHECK(!chart.calls.empty());
    if (!chart.calls.empty()) {
        CHECK(chart.calls[0].cursor.point.interval_index == 2);
        CHECK(chart.calls[0].mark == 110.0);
    }
    // Per sample: the first call at the sample that crossed, 106, sized there
    // -- a smaller slice than the whole-bar one, then whatever the deeper
    // samples still find on the reduced book.
    CHECK(!lower.calls.empty());
    if (!lower.calls.empty() && !chart.calls.empty()) {
        CHECK(lower.calls[0].cursor.point.interval_index == 2);
        CHECK(lower.calls[0].mark == 106.0);
        CHECK(lower.calls[0].units < chart.calls[0].units);
        for (const auto& c : lower.calls) CHECK(c.cursor.point.interval_index == 2);
    }
}

// 3. A host that refuses the new points keeps the old behaviour.
void refused_samples() {
    Probe h;
    h.refuse_samples = true;
    run(h, Path::LowerTf, NativeLiquidationCheck::PathAdverseExtremeMark);
    print("lower_tf, samples refused", h);
    CHECK(h.count(NativeMarginCheckKind::IntrabarSample) > 0);
    CHECK(h.calls.empty());
    CHECK(h.physical_position().signed_units == -174.0);
}

// 4. A one-price synthesized path.
void synthesized_samples() {
    Probe h;
    run(h, Path::Synthesized, NativeLiquidationCheck::PathAdverseExtremeMark);
    print("synthesized, mark check", h);
    CHECK(h.count(NativeMarginCheckKind::BarOpen) == 5);
    CHECK(h.count(NativeMarginCheckKind::IntrabarSample) == 0);
}

}  // namespace

int main() {
    offered_points();
    call_at_the_crossing_sample();
    refused_samples();
    synthesized_samples();
    std::printf("test_native_margin_intrabar_samples: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
