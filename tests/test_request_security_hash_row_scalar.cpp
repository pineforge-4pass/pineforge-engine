// With a request.security site registered, the last recorded broker-state hash
// row of a completed run equals the scalar read after the run
// (include/pineforge/pineforge.h, pf_report_t::broker_state_hash: "the last
// row equals #strategy_broker_state_hash after the run for Pine and bare
// native hosts alike").
//
// Why a site is its own case. Once a site is registered the Pine host runs
// every chart body under a request caller scope (PineRequestCallerScope,
// opened by PineStrategyHost::on_native_bar) and writes that bar's hash row
// before the scope closes. The scope puts the previous caller back on exit, so
// when the scalar is read after the run no caller is active. A row may
// therefore fold only state that outlives its callback: a fold of the active
// caller is in every row and never in the scalar, and the last row then stops
// being the run's terminal witness.
//
// One target: last row == scalar. The checks before it are its preconditions,
// so the comparison cannot pass on a run that never met the case: the run
// completed with one row per chart bar, the site published completed bars,
// and every chart body ran with a bound request caller, which is the state
// each row was written under.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

constexpr std::int64_t kQuarterHour = 15 * 60'000LL;
// 2024-01-01 00:00 UTC. The feed starts on an hour and ends on one, so no
// requested bucket is left open when the run ends.
constexpr std::int64_t kOrigin = 1704067200000LL;

// A 24x7 quarter-hour feed: four chart bars per "60" bucket.
std::vector<Bar> quarter_hour_bars(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double open = 100.0 + i;
        Bar bar{};
        bar.open = open;
        bar.high = open + 2.0;
        bar.low = open - 1.0;
        bar.close = open + 0.5;
        bar.volume = static_cast<double>(i + 1);
        bar.timestamp = kOrigin + static_cast<std::int64_t>(i) * kQuarterHour;
        bars.push_back(bar);
    }
    return bars;
}

// One request.security site, registered the way generated
// configure_security_evaluators() spells it.
class OneSiteProbe final : public source::PineStrategyHost {
public:
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "60", input_tf_, false, false);
    }
    void evaluate_security(int sec_id, const Bar&, bool is_complete) override {
        if (sec_id == 0 && is_complete) ++completed_requested_bars_;
    }
    void on_source_bar(const Bar&) override {
        ++chart_bodies_;
        // The request caller this body runs under; the bar's hash row is
        // written while it is still active.
        if (current_request_read(0).caller_valid) ++bodies_with_request_caller_;
    }

    int completed_requested_bars() const noexcept { return completed_requested_bars_; }
    int chart_bodies() const noexcept { return chart_bodies_; }
    int bodies_with_request_caller() const noexcept { return bodies_with_request_caller_; }

private:
    int completed_requested_bars_ = 0;
    int chart_bodies_ = 0;
    int bodies_with_request_caller_ = 0;
};

}  // namespace

int main() {
    const std::vector<Bar> bars = quarter_hour_bars(16);
    const int bar_count = static_cast<int>(bars.size());

    OneSiteProbe probe;
    probe.set_syminfo_timezone("UTC");
    probe.set_syminfo_session("24x7");
    probe.set_syminfo_type("crypto");
    probe.set_broker_state_hash_recording(true);
    probe.run(bars.data(), bar_count, "15", "15");
    CHECK(probe.last_error().empty());
    if (!probe.last_error().empty())
        std::fprintf(stderr, "  run error: %s\n", probe.last_error().c_str());

    ReportC report{};
    probe.fill_report(&report);
    const std::int64_t rows = report.broker_state_hash_len;

    // Preconditions: a completed run, one row per chart bar, a live site, and
    // a bound request caller under every chart body.
    CHECK(report.script_bars_processed == bar_count);
    CHECK(rows == report.script_bars_processed);
    CHECK(probe.completed_requested_bars() > 0);
    CHECK(probe.chart_bodies() > 0);
    CHECK(probe.bodies_with_request_caller() == probe.chart_bodies());

    // The target: the row written under the last body's request scope is the
    // scalar read after every scope has closed.
    CHECK(rows > 0 && report.broker_state_hash != nullptr);
    if (rows > 0 && report.broker_state_hash != nullptr) {
        const std::uint64_t last_row = report.broker_state_hash[rows - 1];
        const std::uint64_t scalar = probe.broker_state_hash();
        if (last_row != scalar) {
            std::fprintf(stderr,
                         "  last recorded row %016llx, scalar after the run %016llx\n",
                         static_cast<unsigned long long>(last_row),
                         static_cast<unsigned long long>(scalar));
        }
        CHECK(last_row == scalar);
    }
    BacktestEngine::free_report(&report);

    std::printf("test_request_security_hash_row_scalar: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
