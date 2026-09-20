// The adapter twin of test_native_auxiliary_feed.cpp, and the measurement
// behind a ruling: the Pine source host KEEPS its own auxiliary drive
// (PineStrategyHost::set_aux_security_feed, the chart-slice mapping and
// feed_aux_security_for_chart_bar) and does not re-lower its sites onto the
// kernel's generic auxiliary feed.
//
// Both hosts below are driven over the SAME 15-minute chart and the SAME
// 1-minute finer bars. Three rows, each a fact observed on this tree:
//
//   A. congruent shape -- a feed that begins exactly at the first chart bar, a
//      plain lookahead_off site no finer than the chart: the two series are
//      the same buckets, read at the same calculations. The generic feed can
//      express this shape.
//   B. feed history -- the same feed with an hour of bars before the first
//      chart bar: TradingView's chart-slice mapping leaves pre-range coverage
//      inert (and cuts the bucket in progress at the range start), so the
//      adapter publishes two hours; the kernel's routing is by time and folds
//      the history on the first input, so the bare host reads three. Every
//      split-feed lane the adapter serves carries such history.
//   C. the evaluation point -- on EVERY shape, row A's included, the adapter
//      publishes a chart bar's slice at that bar's CALCULATION, after the
//      bar's own matching pass, while the kernel delivers a series on the
//      accepted input, BEFORE it is matched. A position opened at bar 1's open
//      is already there when the adapter evaluates bar 1's bucket and is not
//      yet there when the kernel delivers it. A generated evaluate_security
//      body is opaque to any routing predicate, so no predicate can prove the
//      move neutral, and the corpus holds no auxiliary-feed probe to measure
//      it on (0 of 312).
//
// Rows B and C are why the adapter's auxiliary path is retained. This unit
// names the source layer, so a kernel-only build drops it; the bare host's own
// contract is pinned source-free in test_native_auxiliary_feed{,_stream}.cpp.

#include "native_auxiliary_feed_fixture.hpp"

#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the auxiliary-feed twin requires the source host's split-feed surface"
#endif

namespace {

// What either host saw: every published bucket, the position it was
// published against, and the bucket each chart calculation read.
struct Observed {
    std::vector<Bar> published;
    std::vector<double> position_at_publication;
    std::vector<std::optional<Bar>> read_at_calculation;
};

// The adapter: one plain request.security site, fed by the source host's own
// auxiliary drive.
class AdapterProbe final : public source::PineStrategyHost {
public:
    std::string site_tf = "60";
    Observed seen;
    std::optional<Bar> latest;
    int calculations = 0;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, site_tf, input_tf_, false, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) return;
        latest = bar;
        seen.published.push_back(bar);
        seen.position_at_publication.push_back(live_position_size());
    }
    void on_source_bar(const Bar&) override {
        seen.read_at_calculation.push_back(latest);
        if (calculations++ == 0) strategy_entry("L", true);
    }
};

// The bare host: the same site as a series built from the generic feed.
class BareProbe final : public FeedHost {
public:
    Observed seen;
    int calculations = 0;

    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        FeedHost::on_native_timeframe_bar(bar, context);
        seen.published.push_back(bar);
        seen.position_at_publication.push_back(physical_position().signed_units);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        FeedHost::on_native_bar(bar, context);
        seen.read_at_calculation.push_back(native_series_bar(0));
        if (calculations++ == 0) {
            submit_market({order_action::Transact{1.0}, "twin-long", ""});
        }
    }
};

Observed run_adapter(const std::vector<Bar>& chart, const std::vector<Bar>& finer,
                     const char* site_tf) {
    AdapterProbe probe;
    probe.site_tf = site_tf;
    const auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "UTC");
    strategy_set_syminfo_session(handle, "24x7");
    strategy_set_syminfo_type(handle, "crypto");
    const int installed = strategy_set_aux_security_feed(
        handle, reinterpret_cast<const pf_bar_t*>(finer.data()),
        static_cast<int>(finer.size()), "1");
    CHECK(installed == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    if (!probe.last_error().empty()) std::printf("  adapter: %s\n", probe.last_error().c_str());
    return probe.seen;
}

Observed run_bare(const std::vector<Bar>& chart, const std::vector<Bar>& finer,
                  const char* site_tf, const char* session_key) {
    NativeRunSpec spec = base_spec(session_key);
    spec.auxiliary_feed = minute_feed(finer);
    spec.subscriptions.push_back(series(site_tf, NativeSeriesSource::AuxiliaryFeed));
    BareProbe probe;
    run_batch(probe, spec, chart);
    return probe.seen;
}

void check_same_bars(const std::vector<Bar>& got, const std::vector<Bar>& want,
                     const char* tag) {
    CHECK(got.size() == want.size());
    if (got.size() != want.size()) {
        std::printf("  %s: %zu buckets, want %zu\n", tag, got.size(), want.size());
        return;
    }
    for (std::size_t i = 0; i < got.size(); ++i) check_bucket(got[i], want[i], tag);
}

// ---- A. the congruent shape ------------------------------------------------

void row_a_congruent_shape_is_the_same_series() {
    scenario = "A: congruent shape";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> finer = minute_bars(origin, 120);
    const std::vector<Bar> chart = quarter_bars_over(finer, 8);

    const Observed adapter = run_adapter(chart, finer, "60");
    const Observed bare = run_bare(chart, finer, "60", "native-aux-twin-a");

    const std::vector<Bar> hours = {hand_aggregate(finer, 0, 60, origin),
                                    hand_aggregate(finer, 60, 60, origin + kHour)};
    check_same_bars(adapter.published, hours, "adapter hour");
    check_same_bars(bare.published, hours, "bare hour");
    CHECK(adapter.read_at_calculation.size() == 8);
    CHECK(bare.read_at_calculation.size() == 8);
    for (std::size_t j = 0; j < 8 && j < adapter.read_at_calculation.size()
                            && j < bare.read_at_calculation.size(); ++j) {
        const auto& a = adapter.read_at_calculation[j];
        const auto& b = bare.read_at_calculation[j];
        CHECK(a.has_value() == b.has_value());
        CHECK(a.has_value() == (j >= 3));
        if (a && b) check_bucket(*b, *a, "bare vs adapter at calculation");
    }
}

// ---- B. feed history before the chart --------------------------------------

void row_b_feed_history_is_inert_for_the_adapter_and_folded_by_the_kernel() {
    scenario = "B: feed history";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> finer = minute_bars(origin - kHour, 180);
    const std::vector<Bar> in_range(finer.begin() + 60, finer.end());
    const std::vector<Bar> chart = quarter_bars_over(in_range, 8);

    const Observed adapter = run_adapter(chart, finer, "60");
    const Observed bare = run_bare(chart, finer, "60", "native-aux-twin-b");

    // The adapter: the chart's two hours, the history never published.
    check_same_bars(adapter.published,
                    {hand_aggregate(finer, 60, 60, origin),
                     hand_aggregate(finer, 120, 60, origin + kHour)},
                    "adapter hour");
    // The kernel: the same two hours behind the hour the chart never reached.
    check_same_bars(bare.published,
                    {hand_aggregate(finer, 0, 60, origin - kHour),
                     hand_aggregate(finer, 60, 60, origin),
                     hand_aggregate(finer, 120, 60, origin + kHour)},
                    "bare hour");
    // So the first three calculations already read a bucket on the bare host
    // and none on the adapter.
    CHECK(adapter.read_at_calculation.size() == 8 && bare.read_at_calculation.size() == 8);
    for (std::size_t j = 0; j < 3 && j < adapter.read_at_calculation.size()
                            && j < bare.read_at_calculation.size(); ++j) {
        CHECK(!adapter.read_at_calculation[j].has_value());
        CHECK(bare.read_at_calculation[j].has_value());
    }
}

// ---- C. the evaluation point ------------------------------------------------

void row_c_the_adapter_evaluates_after_matching_the_kernel_delivers_before() {
    scenario = "C: evaluation point";
    const std::int64_t origin = origin_ms();
    const std::vector<Bar> finer = minute_bars(origin, 60);
    const std::vector<Bar> chart = quarter_bars_over(finer, 4);

    // A site of the chart's own period publishes once per chart bar, so the
    // bucket of chart bar 1 is published while bar 1 is the calling bar. The
    // market order both hosts place at bar 0's calculation fills at bar 1's
    // open.
    const Observed adapter = run_adapter(chart, finer, "15");
    const Observed bare = run_bare(chart, finer, "15", "native-aux-twin-c");

    const std::vector<Bar> quarters = {
        hand_aggregate(finer, 0, 15, origin),
        hand_aggregate(finer, 15, 15, origin + kQuarter),
        hand_aggregate(finer, 30, 15, origin + 2 * kQuarter),
        hand_aggregate(finer, 45, 15, origin + 3 * kQuarter),
    };
    check_same_bars(adapter.published, quarters, "adapter quarter");
    check_same_bars(bare.published, quarters, "bare quarter");
    CHECK(adapter.position_at_publication.size() == 4);
    CHECK(bare.position_at_publication.size() == 4);
    if (adapter.position_at_publication.size() != 4
        || bare.position_at_publication.size() != 4) {
        return;
    }
    // Bar 0's bucket: nobody holds anything yet.
    CHECK(adapter.position_at_publication[0] == 0.0);
    CHECK(bare.position_at_publication[0] == 0.0);
    // Bar 1's bucket: the same bars, the same fill at bar 1's open -- already
    // applied when the adapter evaluates, not yet when the kernel delivers.
    CHECK(adapter.position_at_publication[1] == 1.0);
    CHECK(bare.position_at_publication[1] == 0.0);
    // One input later both agree again: the difference is one matching pass.
    CHECK(adapter.position_at_publication[2] == 1.0);
    CHECK(bare.position_at_publication[2] == 1.0);
}

}  // namespace

int main() {
    row_a_congruent_shape_is_the_same_series();
    row_b_feed_history_is_inert_for_the_adapter_and_folded_by_the_kernel();
    row_c_the_adapter_evaluates_after_matching_the_kernel_delivers_before();
    std::printf("test_native_auxiliary_feed_twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
