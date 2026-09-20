// Native calculation timing for a bare NativeStrategyHost: the
// NativeCalculationTrigger cadence, the OrderFill cascade and its per-point
// bound, the EveryModeledPoint tick cadence, the lower-timeframe sub-bar
// hook, the OpenOnly bar-open view and the partial-bar accessor.
//
// Witnesses, all independent of the feature under test:
//   1. the BarClose default delivers exactly the callback sequence, the fills
//      and the continuation hash a clean b01ef03 build delivers, and drives
//      zero recalculations;
//   2. BarCloseAndFills: a host that refills on its own fill produces the
//      cascade entry -> applied -> recalculate -> execute -> applied ... in
//      that order, and max_recalculations_per_point = 2 stops it after two
//      recalculations while the third fill is still applied and delivered;
//   3. EveryModeledPoint: one Tick recalculation per modeled point in batch
//      (each confirmed waypoint, each intrabar sample) and one per observed
//      print in a stream, in order, with current_partial_bar() folding the
//      bar so far up to that cursor;
//   4. on_native_sub_bar fires once per retained lower-timeframe sub-bar and
//      never for a plain confirmed or synthesized path;
//   5. OpenOnly hands the bar-open callback H = L = C = open and volume 0
//      while the applied and close callbacks keep the complete bar, and the
//      fills are exactly the ones Complete books;
//   6. recalculations record no report point: under KernelRecorded the curve
//      still has one point per script bar;
//   7. a default spec hashes to the pre-L5 continuation constant;
//   8. TWIN: the adapter's calc_on_order_fills probe from
//      tests/test_native_l4c_coof_literals.cpp, re-expressed natively — in
//      tests/test_native_calc_timing_twin.cpp, which binds pineforge/source.
//      This TU is source-free and runs in the kernel-only profile.

#include "native_calc_timing_fixture.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace l5_fixture;

// ---- 1. the BarClose default surface --------------------------------------

// The cadence folds into the run-spec fold only once the trigger or the
// open-bar view is non-default, so a BarClose spec folds exactly the pre-L5
// fields. That is pinned as native_run_spec_digest() and not as a continuation
// hash: the continuation identity also folds the machine's resolved timezone
// resources (zoneinfo root and zone file paths), so a raw constant passes here
// and fails on CI. Observed on THIS tree for base_spec("native-calc-timing");
// it guards the fold's field list and order, while the neutrality claim is the
// equalities in test_bar_close_default — including the callback log, the book
// and the recalculation counters, which are the pre-L5 contract itself.
constexpr std::uint64_t kDefaultSpecDigest = 12300031127478902007ull;

class DefaultHost final : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    int bars = 0;

    void on_native_bar_open(const Bar& bar, const NativeDecisionContext&) override {
        log.push_back("open@" + stamp(bar.timestamp) + " " + fmt(bar.open) + "/"
                      + fmt(bar.high) + "/" + fmt(bar.low) + "/" + fmt(bar.close));
    }
    void on_native_applied(const native_order::ExecutionAppliedEvent& applied,
                           const NativeDecisionContext&) override {
        log.push_back("applied#" + std::to_string(static_cast<unsigned long long>(applied.ordinal))
                      + " px=" + fmt(applied.resolved_price)
                      + " t=" + stamp(applied.effective_time_ms()));
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bars;
        log.push_back("bar@" + stamp(bar.timestamp));
        if (bars == 1) submit_market({order_action::Transact{1.0}, "e", ""});
        if (bars == 4) submit_market({execution::Flatten{}, "x", ""});
    }
};

void test_bar_close_default() {
    scenario = "BarClose default";
    const auto bars = minute_bars(6);
    DefaultHost host;
    CHECK(host.configure_native(base_spec("native-calc-timing")).status
          == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());

    const std::vector<std::string> want = {
        "open@0 100.0000/102.0000/99.0000/101.0000",
        "bar@0",
        "open@60000 101.0000/103.0000/100.0000/102.0000",
        "applied#8 px=101.0000 t=60000",
        "bar@60000",
        "open@120000 102.0000/104.0000/101.0000/103.0000",
        "bar@120000",
        "open@180000 103.0000/105.0000/102.0000/104.0000",
        "bar@180000",
        "open@240000 104.0000/106.0000/103.0000/105.0000",
        "applied#26 px=104.0000 t=240000",
        "bar@240000",
        "open@300000 105.0000/107.0000/104.0000/106.0000",
        "bar@300000",
    };
    report_log(host.log, want);
    CHECK(host.log == want);
    CHECK(host.physical_position().lot_count == 0);
    CHECK(host.closed_trade_count() == 1);
    // The default cadence drives no recalculation at all: every calculation
    // of the run is the script bar's own.
    CHECK(host.native_recalculation_count() == 0);
    CHECK(host.native_recalculations_skipped() == 0);
    // 7. hash neutrality against the pre-L5 tip, stated portably.
    const auto base = base_spec("native-calc-timing");
    const auto digest = native_run_spec_digest(base);
    if (digest != kDefaultSpecDigest) {
        std::printf("  spec digest %llu, pinned %llu\n",
                    static_cast<unsigned long long>(digest),
                    static_cast<unsigned long long>(kDefaultSpecDigest));
    }
    CHECK(digest == kDefaultSpecDigest);

    // Spelling all three cadence fields out at their defaults folds nothing,
    // and the recalculation bound is inert while the cadence is BarClose.
    auto stated = base;
    stated.calculation = NativeCalculationTrigger::BarClose;
    stated.max_recalculations_per_point = 8;
    stated.open_bar_view = NativeOpenBarView::Complete;
    CHECK(native_run_spec_digest(stated) == kDefaultSpecDigest);
    auto bound_only = base;
    bound_only.max_recalculations_per_point = 3;
    CHECK(native_run_spec_digest(bound_only) == kDefaultSpecDigest);

    // Moving the trigger, or the open-bar view on its own, is what moves it.
    auto moved = base;
    moved.calculation = NativeCalculationTrigger::BarCloseAndFills;
    CHECK(native_run_spec_digest(moved) != kDefaultSpecDigest);
    auto open_only = base;
    open_only.open_bar_view = NativeOpenBarView::OpenOnly;
    CHECK(native_run_spec_digest(open_only) != kDefaultSpecDigest);

    // The same three facts at run level, compared between runs in this process
    // so no continuation constant is needed: the restated defaults reproduce
    // this run's callback log and identity, and the opted-in spec cannot share
    // that identity.
    DefaultHost restated;
    CHECK(restated.configure_native(stated).status == NativeSetupStatus::Applied);
    restated.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
                 MagnifierDistribution::ENDPOINTS);
    CHECK(restated.last_error().empty());
    CHECK(restated.log == want);
    CHECK(restated.native_recalculation_count() == 0);
    CHECK(restated.native_recalculations_skipped() == 0);
    CHECK(restated.native_continuation_hash() == host.native_continuation_hash());

    DefaultHost opted;
    CHECK(opted.configure_native(moved).status == NativeSetupStatus::Applied);
    opted.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(opted.last_error().empty());
    CHECK(opted.native_continuation_hash() != host.native_continuation_hash());
}

// The BarClose calculation reaches on_native_recalculate too, and its default
// forwarding is what a host that never overrode it already sees.
class RoutedHost final : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason reason,
                               const native_order::ExecutionAppliedEvent* cause) override {
        log.push_back("recalc reason=" + std::to_string(static_cast<int>(reason))
                      + " cause=" + (cause ? "yes" : "no")
                      + " t=" + stamp(ctx.coordinate.effective_time_ms));
        NativeStrategyHost::on_native_recalculate(bar, ctx, reason, cause);
    }
    int forwarded = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++forwarded; }
};

void test_every_calculation_is_routed() {
    scenario = "calculation routing";
    const auto bars = minute_bars(3);
    RoutedHost host;
    CHECK(host.configure_native(base_spec("native-calc-routing")).status
          == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    const std::vector<std::string> want = {
        "recalc reason=0 cause=no t=60000",
        "recalc reason=0 cause=no t=120000",
        "recalc reason=0 cause=no t=180000",
    };
    report_log(host.log, want);
    CHECK(host.log == want);
    CHECK(host.forwarded == 3);
}

// ---- 2. the fill cascade and its per-point bound ---------------------------

class CascadeHost final : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    int bars = 0;
    double target_units = 4.0;

    void on_native_applied(const native_order::ExecutionAppliedEvent& applied,
                           const NativeDecisionContext&) override {
        log.push_back("applied#" + std::to_string(static_cast<unsigned long long>(applied.ordinal))
                      + " px=" + fmt(applied.resolved_price));
    }
    void on_native_recalculate(const Bar&, const NativeDecisionContext&,
                               NativeCalculationReason reason,
                               const native_order::ExecutionAppliedEvent* cause) override {
        if (reason == NativeCalculationReason::BarClose) {
            if (++bars == 1) submit_market({order_action::Transact{1.0}, "seed", ""});
            return;
        }
        CHECK(reason == NativeCalculationReason::OrderFill);
        CHECK(cause != nullptr);
        log.push_back("recalc cause#"
                      + std::to_string(cause ? static_cast<unsigned long long>(cause->ordinal) : 0ull)
                      + " units=" + fmt(physical_position().signed_units));
        if (std::abs(physical_position().signed_units) >= target_units) return;
        const auto submitted = submit_market({order_action::Transact{1.0}, "refill", ""});
        CHECK(submitted.handle.has_value());
        if (!submitted.handle) return;
        const auto outcome = execute_current({*submitted.handle,
                                              NativeCurrentPriceRule::AsPresented});
        CHECK(std::holds_alternative<native_order::ExecutionAppliedEvent>(outcome));
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

void test_fill_cascade() {
    scenario = "BarCloseAndFills cascade";
    const auto bars = minute_bars(4);
    auto spec = base_spec("native-calc-cascade");
    spec.calculation = NativeCalculationTrigger::BarCloseAndFills;

    // (a) a budget the host's own rule never reaches.
    CascadeHost wide;
    CHECK(wide.configure_native(spec).status == NativeSetupStatus::Applied);
    wide.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(wide.last_error().empty());
    const std::vector<std::string> want_wide = {
        "applied#8 px=101.0000",
        "recalc cause#8 units=1.0000",
        "applied#11 px=101.0000",
        "recalc cause#11 units=2.0000",
        "applied#14 px=101.0000",
        "recalc cause#14 units=3.0000",
        "applied#17 px=101.0000",
        "recalc cause#17 units=4.0000",
    };
    report_log(wide.log, want_wide);
    CHECK(wide.log == want_wide);
    CHECK(same(wide.physical_position().signed_units, 4.0));
    CHECK(wide.native_recalculation_count() == 4);
    CHECK(wide.native_recalculations_skipped() == 0);

    // (b) the same rule against a two-recalculation budget: the third fill is
    // still applied and still delivered, it just drives no calculation.
    auto bounded_spec = spec;
    bounded_spec.max_recalculations_per_point = 2;
    CascadeHost bounded;
    CHECK(bounded.configure_native(bounded_spec).status == NativeSetupStatus::Applied);
    bounded.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
                MagnifierDistribution::ENDPOINTS);
    CHECK(bounded.last_error().empty());
    const std::vector<std::string> want_bounded = {
        "applied#8 px=101.0000",
        "recalc cause#8 units=1.0000",
        "applied#11 px=101.0000",
        "recalc cause#11 units=2.0000",
        "applied#14 px=101.0000",
    };
    report_log(bounded.log, want_bounded);
    CHECK(bounded.log == want_bounded);
    CHECK(same(bounded.physical_position().signed_units, 3.0));
    CHECK(bounded.physical_position().lot_count == 3);
    CHECK(bounded.native_recalculation_count() == 2);
    CHECK(bounded.native_recalculations_skipped() == 1);

    // (c) the same host under the default trigger: the fills the seed order
    // books are delivered, and not one of them recalculates.
    CascadeHost closed;
    CHECK(closed.configure_native(base_spec("native-calc-cascade-off")).status
          == NativeSetupStatus::Applied);
    closed.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(closed.last_error().empty());
    CHECK(closed.log == std::vector<std::string>{"applied#8 px=101.0000"});
    CHECK(same(closed.physical_position().signed_units, 1.0));
    CHECK(closed.native_recalculation_count() == 0);
    CHECK(closed.native_recalculations_skipped() == 0);
}

// ---- 3/4. every modeled point, and the sub-bar hook ------------------------

class PointHost final : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    int ticks = 0;
    int sub_bars = 0;

    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason reason,
                               const native_order::ExecutionAppliedEvent*) override {
        if (reason == NativeCalculationReason::Tick) ++ticks;
        log.push_back(std::string(reason == NativeCalculationReason::BarClose ? "close" : "tick")
                      + " t=" + stamp(ctx.coordinate.effective_time_ms)
                      + " bar.c=" + fmt(bar.close) + " partial=" + partial_of(*this));
    }
    void on_native_sub_bar(const Bar& sub, const NativeDecisionContext& ctx) override {
        ++sub_bars;
        log.push_back("sub t=" + stamp(sub.timestamp) + " idx=" + std::to_string(ctx.sub_index)
                      + " partial=" + partial_of(*this));
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

void test_every_modeled_point_confirmed() {
    scenario = "EveryModeledPoint, confirmed path";
    const auto bars = minute_bars(2);
    auto spec = base_spec("native-calc-points-plain");
    spec.calculation = NativeCalculationTrigger::EveryModeledPoint;
    PointHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());

    // Four modeled points per confirmed bar (opening, low leg, high leg,
    // close leg) and then the bar's own calculation, whose partial is gone
    // because the host already holds the complete bar.
    const std::vector<std::string> want = {
        "tick t=0 bar.c=101.0000 partial=100.0000/100.0000/100.0000/100.0000 v=0.0000",
        "tick t=0 bar.c=101.0000 partial=100.0000/100.0000/99.0000/99.0000 v=0.0000",
        "tick t=0 bar.c=101.0000 partial=100.0000/102.0000/99.0000/102.0000 v=0.0000",
        "tick t=60000 bar.c=101.0000 partial=100.0000/102.0000/99.0000/101.0000 v=0.0000",
        "close t=60000 bar.c=101.0000 partial=none",
        "tick t=60000 bar.c=102.0000 partial=101.0000/101.0000/101.0000/101.0000 v=0.0000",
        "tick t=60000 bar.c=102.0000 partial=101.0000/101.0000/100.0000/100.0000 v=0.0000",
        "tick t=60000 bar.c=102.0000 partial=101.0000/103.0000/100.0000/103.0000 v=0.0000",
        "tick t=120000 bar.c=102.0000 partial=101.0000/103.0000/100.0000/102.0000 v=0.0000",
        "close t=120000 bar.c=102.0000 partial=none",
    };
    report_log(host.log, want);
    CHECK(host.log == want);
    CHECK(host.ticks == 8);
    CHECK(host.native_recalculation_count() == 8);
    // 4. no retained lower feed, so the sub-bar hook never fires.
    CHECK(host.sub_bars == 0);
}

void test_every_modeled_point_intrabar() {
    scenario = "EveryModeledPoint, lower-timeframe path";
    const auto lower = minute_bars(6);
    auto spec = base_spec("native-calc-points-lower", "3");
    spec.calculation = NativeCalculationTrigger::EveryModeledPoint;
    IntrabarPath::lower_tf path;
    path.bars = lower;
    path.tf = "1";
    path.samples = 4;
    spec.intrabar.value = path;
    PointHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(lower.data(), static_cast<int>(lower.size()), "1", "3", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());

    // Two three-minute script bars, three retained sub-bars each, four
    // sampled points per sub-bar: 24 Tick recalculations and 6 sub-bar hooks.
    CHECK(host.ticks == 24);
    CHECK(host.native_recalculation_count() == 24);
    CHECK(host.sub_bars == 6);

    // The first sub-bar's own walk, in order, with the bar so far following
    // the cursor and its volume accruing only once the sub-bar completes.
    const std::vector<std::string> head = {
        "tick t=0 bar.c=103.0000 partial=100.0000/100.0000/100.0000/100.0000 v=0.0000",
        "tick t=0 bar.c=103.0000 partial=100.0000/100.0000/99.0000/99.0000 v=0.0000",
        "tick t=0 bar.c=103.0000 partial=100.0000/102.0000/99.0000/102.0000 v=0.0000",
        "tick t=0 bar.c=103.0000 partial=100.0000/102.0000/99.0000/101.0000 v=0.0000",
        "sub t=0 idx=0 partial=100.0000/102.0000/99.0000/101.0000 v=10.0000",
        "tick t=60000 bar.c=103.0000 partial=100.0000/102.0000/99.0000/101.0000 v=10.0000",
    };
    std::vector<std::string> got(host.log.begin(),
                                 host.log.begin() + static_cast<long>(head.size()));
    report_log(got, head);
    CHECK(got == head);
    // Each script bar's calculation still happens exactly once, after its
    // last sub-bar, with no partial of its own.
    CHECK(host.log[14] == "sub t=120000 idx=2 partial=100.0000/104.0000/99.0000/103.0000 v=33.0000");
    CHECK(host.log[15] == "close t=180000 bar.c=103.0000 partial=none");
    CHECK(host.log.size() == 32);
    CHECK(host.log[31] == "close t=360000 bar.c=106.0000 partial=none");
}

void test_sub_bar_hook_needs_a_lower_feed() {
    scenario = "sub-bar hook";
    const auto bars = minute_bars(6);
    // A synthesized path samples the script bar's own OHLC: it retains no
    // lower bars, so it has no sub-bars of its own to deliver.
    auto spec = base_spec("native-calc-synth", "3");
    IntrabarPath::synthesized synthesized;
    synthesized.samples = 4;
    spec.intrabar.value = synthesized;
    PointHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "3", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    CHECK(host.sub_bars == 0);
    // The cadence is untouched by the hook: BarClose still calculates once
    // per script bar and drives no recalculation.
    CHECK(host.ticks == 0);
    CHECK(host.native_recalculation_count() == 0);

    // The same lower feed, at the default trigger: the hook is not a cadence
    // and fires for every retained sub-bar anyway.
    auto lower_spec = base_spec("native-calc-lower-default", "3");
    IntrabarPath::lower_tf path;
    path.bars = bars;
    path.tf = "1";
    path.samples = 4;
    lower_spec.intrabar.value = path;
    PointHost lower_host;
    CHECK(lower_host.configure_native(lower_spec).status == NativeSetupStatus::Applied);
    lower_host.run(bars.data(), static_cast<int>(bars.size()), "1", "3", false, 4,
                   MagnifierDistribution::ENDPOINTS);
    CHECK(lower_host.last_error().empty());
    CHECK(lower_host.sub_bars == 6);
    CHECK(lower_host.ticks == 0);
    CHECK(lower_host.native_recalculation_count() == 0);
}

// ---- 3 (stream half). one Tick recalculation per observed print ------------

class StreamHost final : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    void on_native_tick(const Bar& bar, const NativeTickContext&) override {
        log.push_back("observed " + fmt(bar.close) + " partial=" + partial_of(*this));
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext&,
                               NativeCalculationReason reason,
                               const native_order::ExecutionAppliedEvent*) override {
        if (reason == NativeCalculationReason::BarClose) {
            log.push_back("close " + fmt(bar.close));
            return;
        }
        log.push_back("tick " + fmt(bar.close) + " partial=" + partial_of(*this));
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

void test_every_modeled_point_stream() {
    scenario = "EveryModeledPoint, observed prints";
    auto spec = base_spec("native-calc-stream");
    spec.calculation = NativeCalculationTrigger::EveryModeledPoint;
    StreamHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const Bar warmup{100.0, 100.0, 100.0, 100.0, 1.0, -60000};
    CHECK(host.stream_begin(&warmup, 1, "1", "1"));
    const std::size_t after_warmup = host.log.size();
    const TradeTick ticks[] = {{0, 1, 100.5, 2.0}, {10000, 2, 101.5, 3.0},
                               {20000, 3, 99.5, 1.0}, {60000, 4, 102.0, 4.0}};
    for (const auto& tick : ticks) CHECK(host.stream_push_tick(tick));
    CHECK(host.stream_end(true));
    CHECK(host.last_error().empty());

    // The observation hook stays ahead of the recalculation: each print is
    // observed, matched, then recalculated once, with the bar so far carrying
    // that print's price and its traded quantity.
    const std::vector<std::string> want = {
        "observed 100.5000 partial=100.5000/100.5000/100.5000/100.5000 v=2.0000",
        "tick 100.5000 partial=100.5000/100.5000/100.5000/100.5000 v=2.0000",
        "observed 101.5000 partial=100.5000/101.5000/100.5000/101.5000 v=5.0000",
        "tick 101.5000 partial=100.5000/101.5000/100.5000/101.5000 v=5.0000",
        "observed 99.5000 partial=100.5000/101.5000/99.5000/99.5000 v=6.0000",
        "tick 99.5000 partial=100.5000/101.5000/99.5000/99.5000 v=6.0000",
        "close 99.5000",
        "observed 102.0000 partial=102.0000/102.0000/102.0000/102.0000 v=4.0000",
        "tick 102.0000 partial=102.0000/102.0000/102.0000/102.0000 v=4.0000",
        "close 102.0000",
    };
    std::vector<std::string> got(host.log.begin() + static_cast<long>(after_warmup),
                                 host.log.end());
    report_log(got, want);
    CHECK(got == want);
}

// ---- 5. the open-bar view --------------------------------------------------

class ViewHost final : public NativeStrategyHost {
public:
    std::vector<std::string> log;
    std::vector<double> fills;
    int bars = 0;

    static std::string shape(const Bar& bar) {
        return fmt(bar.open) + "/" + fmt(bar.high) + "/" + fmt(bar.low) + "/"
            + fmt(bar.close) + " v=" + fmt(bar.volume);
    }
    void on_native_bar_open(const Bar& bar, const NativeDecisionContext&) override {
        log.push_back("open " + shape(bar) + " partial=" + partial_of(*this));
    }
    void on_native_applied(const native_order::ExecutionAppliedEvent& applied,
                           const NativeDecisionContext&) override {
        fills.push_back(applied.resolved_price);
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason reason,
                               const native_order::ExecutionAppliedEvent* cause) override {
        if (reason != NativeCalculationReason::BarClose) {
            log.push_back("fill-recalc " + shape(bar));
            return;
        }
        log.push_back("bar " + shape(bar));
        if (++bars == 1) submit_market({order_action::Transact{1.0}, "e", ""});
        (void)ctx;
        (void)cause;
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

void test_open_bar_view() {
    scenario = "open-bar view";
    const auto bars = minute_bars(3);
    auto complete_spec = base_spec("native-calc-view");
    complete_spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
    ViewHost complete;
    CHECK(complete.configure_native(complete_spec).status == NativeSetupStatus::Applied);
    complete.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
                 MagnifierDistribution::ENDPOINTS);
    CHECK(complete.last_error().empty());

    auto open_only_spec = complete_spec;
    open_only_spec.open_bar_view = NativeOpenBarView::OpenOnly;
    ViewHost open_only;
    CHECK(open_only.configure_native(open_only_spec).status == NativeSetupStatus::Applied);
    open_only.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
                  MagnifierDistribution::ENDPOINTS);
    CHECK(open_only.last_error().empty());

    const std::vector<std::string> want_complete = {
        "open 100.0000/102.0000/99.0000/101.0000 v=10.0000 partial=100.0000/100.0000/100.0000/100.0000 v=0.0000",
        "bar 100.0000/102.0000/99.0000/101.0000 v=10.0000",
        "open 101.0000/103.0000/100.0000/102.0000 v=11.0000 partial=101.0000/101.0000/101.0000/101.0000 v=0.0000",
        "fill-recalc 101.0000/103.0000/100.0000/102.0000 v=11.0000",
        "bar 101.0000/103.0000/100.0000/102.0000 v=11.0000",
        "open 102.0000/104.0000/101.0000/103.0000 v=12.0000 partial=102.0000/102.0000/102.0000/102.0000 v=0.0000",
        "bar 102.0000/104.0000/101.0000/103.0000 v=12.0000",
    };
    report_log(complete.log, want_complete);
    CHECK(complete.log == want_complete);

    // OpenOnly masks exactly the bar-open callback: H = L = C = open and no
    // volume. The applied recalculation and the close calculation keep the
    // complete bar, and current_partial_bar() is the same either way.
    const std::vector<std::string> want_open_only = {
        "open 100.0000/100.0000/100.0000/100.0000 v=0.0000 partial=100.0000/100.0000/100.0000/100.0000 v=0.0000",
        "bar 100.0000/102.0000/99.0000/101.0000 v=10.0000",
        "open 101.0000/101.0000/101.0000/101.0000 v=0.0000 partial=101.0000/101.0000/101.0000/101.0000 v=0.0000",
        "fill-recalc 101.0000/103.0000/100.0000/102.0000 v=11.0000",
        "bar 101.0000/103.0000/100.0000/102.0000 v=11.0000",
        "open 102.0000/102.0000/102.0000/102.0000 v=0.0000 partial=102.0000/102.0000/102.0000/102.0000 v=0.0000",
        "bar 102.0000/104.0000/101.0000/103.0000 v=12.0000",
    };
    report_log(open_only.log, want_open_only);
    CHECK(open_only.log == want_open_only);

    // Masking a view books no different trade.
    CHECK(complete.fills == open_only.fills);
    CHECK(complete.fills.size() == 1);
    CHECK(same(complete.physical_position().signed_units,
               open_only.physical_position().signed_units));
}

// ---- 6. recalculations record no report point ------------------------------

class ReportHost final : public NativeStrategyHost {
public:
    int bars = 0;
    void on_native_recalculate(const Bar&, const NativeDecisionContext&,
                               NativeCalculationReason reason,
                               const native_order::ExecutionAppliedEvent*) override {
        if (reason == NativeCalculationReason::BarClose) {
            if (++bars == 1) submit_market({order_action::Transact{1.0}, "seed", ""});
            return;
        }
        if (std::abs(physical_position().signed_units) >= 4.0) return;
        const auto submitted = submit_market({order_action::Transact{1.0}, "refill", ""});
        if (!submitted.handle) return;
        (void)execute_current({*submitted.handle, NativeCurrentPriceRule::AsPresented});
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

struct Report {
    ReportC c{};
    explicit Report(const BacktestEngine& engine) { engine.fill_report(&c); }
    ~Report() { BacktestEngine::free_report(&c); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

void test_recalculations_add_no_report_points() {
    scenario = "kernel-recorded report";
    const auto bars = minute_bars(6);
    auto spec = base_spec("native-calc-report");
    spec.calculation = NativeCalculationTrigger::EveryModeledPoint;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    ReportHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    CHECK(host.native_recalculation_count() > 6);
    const Report report(host);
    CHECK(report.c.script_bars_processed == 6);
    CHECK(report.c.equity_curve_len == 6);
    CHECK(report.c.equity_curve_len == report.c.script_bars_processed);
}

}  // namespace

int main() {
    test_bar_close_default();
    test_every_calculation_is_routed();
    test_fill_cascade();
    test_every_modeled_point_confirmed();
    test_every_modeled_point_intrabar();
    test_sub_bar_hook_needs_a_lower_feed();
    test_every_modeled_point_stream();
    test_open_bar_view();
    test_recalculations_add_no_report_points();
    std::printf("native calculation timing: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
