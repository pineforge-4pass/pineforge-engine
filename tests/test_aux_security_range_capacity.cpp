// The auxiliary chart ranges are real source-host state: this test drives the
// public run entry and C ABI feed setter, then observes the protected vectors
// through a test-only subclass seam. The seam cannot report a constant zero:
// on_source_bar records the production capacity while the run is using it.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the auxiliary-range capacity test drives the source auxiliary feed"
#endif

namespace {
using namespace pineforge;

int checks = 0;
int failures = 0;
const char* scenario = "";

#define CHECK(condition) do {                                                   \
    ++checks;                                                                   \
    if (!(condition)) {                                                         \
        ++failures;                                                             \
        std::fprintf(stderr, "%s:%d [%s] %s\n", __FILE__, __LINE__, scenario,  \
                     #condition);                                              \
    }                                                                            \
} while (0)

constexpr std::int64_t kStart = 1704067200000LL;
constexpr std::int64_t kMinute = 60000LL;
constexpr std::int64_t kQuarterHour = 15 * kMinute;

std::vector<Bar> make_chart(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const double open = 100.0 + static_cast<double>(index % 11) * 0.25;
        bars.push_back({open, open + 1.0, open - 1.0, open + 0.5, 10.0,
                        kStart + static_cast<std::int64_t>(index) * kQuarterHour});
    }
    return bars;
}

std::vector<Bar> make_auxiliary(int chart_count) {
    const int count = chart_count * 15;
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const double open = 90.0 + static_cast<double>(index % 17) * 0.125;
        bars.push_back({open, open + 0.5, open - 0.5, open + 0.25, 1.0,
                        kStart + static_cast<std::int64_t>(index) * kMinute});
    }
    return bars;
}

class AuxRangeProbe final : public source::PineStrategyHost {
public:
    AuxRangeProbe() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.src_series_active = false;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "60", input_tf_, false, false);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) return;
        ++complete_security_bars_;
        last_security_close_ = bar.close;
    }

    void on_source_bar(const Bar&) override {
        peak_begin_capacity_ =
            std::max(peak_begin_capacity_, aux_security_chart_begin_.capacity());
        peak_end_capacity_ =
            std::max(peak_end_capacity_, aux_security_chart_end_.capacity());
        prepared_size_ = std::max(prepared_size_, aux_security_chart_begin_.size());
        prepared_size_ = std::max(prepared_size_, aux_security_chart_end_.size());

        const int index = pine_bar_index();
        if (index == 8 && complete_security_bars_ > 0 && last_security_close_ > 0.0) {
            strategy_entry("aux-long", true);
            entry_issued_ = true;
        }
        if (index == pine_last_bar_index() - 1 && entry_issued_
            && last_security_close_ > 0.0) {
            strategy_close("aux-long");
            exit_issued_ = true;
        }
    }

    std::size_t terminal_begin_capacity() const noexcept {
        return aux_security_chart_begin_.capacity();
    }
    std::size_t terminal_end_capacity() const noexcept {
        return aux_security_chart_end_.capacity();
    }
    std::size_t peak_begin_capacity() const noexcept { return peak_begin_capacity_; }
    std::size_t peak_end_capacity() const noexcept { return peak_end_capacity_; }
    std::size_t prepared_size() const noexcept { return prepared_size_; }
    std::size_t complete_security_bars() const noexcept {
        return complete_security_bars_;
    }
    bool entry_issued() const noexcept { return entry_issued_; }
    bool exit_issued() const noexcept { return exit_issued_; }
    const std::vector<std::uint64_t>& hashes() const noexcept {
        return broker_state_hashes_;
    }

private:
    std::size_t peak_begin_capacity_ = 0;
    std::size_t peak_end_capacity_ = 0;
    std::size_t prepared_size_ = 0;
    std::size_t complete_security_bars_ = 0;
    double last_security_close_ = 0.0;
    bool entry_issued_ = false;
    bool exit_issued_ = false;
};

bool same_bits(double left, double right) {
    std::uint64_t left_bits = 0;
    std::uint64_t right_bits = 0;
    std::memcpy(&left_bits, &left, sizeof(left_bits));
    std::memcpy(&right_bits, &right, sizeof(right_bits));
    return left_bits == right_bits;
}

bool same_trade(const Trade& left, const Trade& right) {
    return left.entry_time == right.entry_time
        && left.exit_time == right.exit_time
        && same_bits(left.entry_price, right.entry_price)
        && same_bits(left.exit_price, right.exit_price)
        && same_bits(left.qty, right.qty)
        && same_bits(left.pnl, right.pnl)
        && same_bits(left.pnl_pct, right.pnl_pct)
        && left.is_long == right.is_long
        && left.entry_bar_index == right.entry_bar_index
        && left.exit_bar_index == right.exit_bar_index
        && left.entry_id == right.entry_id
        && left.entry_comment == right.entry_comment
        && left.exit_comment == right.exit_comment
        && left.exit_id == right.exit_id
        && left.exit_from_bracket == right.exit_from_bracket
        && same_bits(left.max_runup, right.max_runup)
        && same_bits(left.max_drawdown, right.max_drawdown)
        && same_bits(left.commission, right.commission)
        && left.entry_incarnation == right.entry_incarnation
        && left.open_at_end == right.open_at_end
        && left.close_cause == right.close_cause;
}

struct RunResult {
    std::size_t terminal_begin_capacity = 0;
    std::size_t terminal_end_capacity = 0;
    std::size_t peak_begin_capacity = 0;
    std::size_t peak_end_capacity = 0;
    std::size_t prepared_size = 0;
    std::size_t complete_security_bars = 0;
    bool entry_issued = false;
    bool exit_issued = false;
    std::uint64_t final_hash = 0;
    std::vector<std::uint64_t> hashes;
    std::vector<Trade> trades;
};

RunResult run_case(int chart_count) {
    const auto chart = make_chart(chart_count);
    const auto auxiliary = make_auxiliary(chart_count);
    AuxRangeProbe probe;
    const pf_strategy_t handle = static_cast<pf_strategy_t>(&probe);
    CHECK(strategy_set_aux_security_feed(
              handle, reinterpret_cast<const pf_bar_t*>(auxiliary.data()),
              static_cast<int>(auxiliary.size()), "1") == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15");

    RunResult result;
    result.terminal_begin_capacity = probe.terminal_begin_capacity();
    result.terminal_end_capacity = probe.terminal_end_capacity();
    result.peak_begin_capacity = probe.peak_begin_capacity();
    result.peak_end_capacity = probe.peak_end_capacity();
    result.prepared_size = probe.prepared_size();
    result.complete_security_bars = probe.complete_security_bars();
    result.entry_issued = probe.entry_issued();
    result.exit_issued = probe.exit_issued();
    result.final_hash = probe.broker_state_hash();
    result.hashes = probe.hashes();
    for (int index = 0; index < probe.trade_count(); ++index)
        result.trades.push_back(probe.get_trade(index));

    CHECK(probe.last_error().empty());
    CHECK(result.prepared_size == static_cast<std::size_t>(chart_count));
    CHECK(result.peak_begin_capacity >= static_cast<std::size_t>(chart_count));
    CHECK(result.peak_end_capacity >= static_cast<std::size_t>(chart_count));
    CHECK(result.complete_security_bars > 0);
    CHECK(result.entry_issued && result.exit_issued);
    CHECK(result.trades.size() == 1);
    CHECK(!result.hashes.empty());
    return result;
}

void terminal_ranges_release_without_output_drift() {
    for (const int chart_count : {64, 512, 2048}) {
        scenario = "auxiliary chart ranges: terminal release";
        const RunResult reference = run_case(chart_count);
        const RunResult actual = run_case(chart_count);

        CHECK(actual.terminal_begin_capacity == 0);
        CHECK(actual.terminal_end_capacity == 0);
        CHECK(reference.final_hash == actual.final_hash);
        CHECK(reference.hashes == actual.hashes);
        CHECK(reference.trades.size() == actual.trades.size());
        for (std::size_t index = 0;
             index < reference.trades.size() && index < actual.trades.size(); ++index) {
            CHECK(same_trade(reference.trades[index], actual.trades[index]));
        }
    }
}

}  // namespace

int main() {
    terminal_ranges_release_without_output_drift();
    std::printf("test_aux_security_range_capacity: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
