// L4e H01: the warmup trade-start gate must not consume the entry provenance
// exported through the report-row C ABI.  These are the three diagnostic H01
// first-entry tape coordinates; the hosts deliberately emit pre-window stop
// commands, then the matching first in-window entry/close pair.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);         \
    }                                                                           \
} while (0)

constexpr std::int64_t kStepMs = 15 * 60 * 1000;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class H01Probe final : public source::PineStrategyHost {
public:
    H01Probe(std::int64_t first_signal_ms, std::string entry_id, bool is_long)
        : first_signal_ms_(first_signal_ms), entry_id_(std::move(entry_id)), is_long_(is_long) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar& bar) override {
        // The legacy gate admits one preceding script bar.  Keep that bar
        // quiet; every older warmup command must be ignored and must not
        // advance the physical-entry provenance counter.
        if (bar.timestamp < first_signal_ms_ - kStepMs) {
            strategy_entry("warmup", true, kNaN, 1000000.0, 1.0);
            return;
        }
        if (bar.timestamp == first_signal_ms_) {
            strategy_entry(entry_id_, is_long_, kNaN, kNaN, 1.0);
            return;
        }
        if (!submitted_close_ && bar.timestamp > first_signal_ms_
            && signed_position_size() != 0.0) {
            submitted_close_ = true;
            strategy_close(entry_id_);
        }
    }

private:
    std::int64_t first_signal_ms_ = 0;
    std::string entry_id_;
    bool is_long_ = true;
    bool submitted_close_ = false;
};

void check_h01_tape(const char* name, std::int64_t first_signal_ms,
                    const char* entry_id, bool is_long) {
    H01Probe probe(first_signal_ms, entry_id, is_long);
    probe.set_trade_start_time(first_signal_ms);
    std::vector<Bar> bars;
    for (int offset = -14; offset <= 3; ++offset) {
        const auto timestamp = first_signal_ms + static_cast<std::int64_t>(offset) * kStepMs;
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0, timestamp});
    }
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    CHECK(probe.report_trade_count() == 1);
    if (probe.trade_count() != 1 || probe.report_trade_count() != 1) return;

    const Trade& row = probe.get_trade(0);
    CHECK(row.entry_id == entry_id);
    CHECK(row.is_long == is_long);
    CHECK(row.entry_time == first_signal_ms + kStepMs);
    CHECK(row.entry_incarnation == 1);
    const auto handle = reinterpret_cast<pf_strategy_t>(&probe);
    CHECK(strategy_closed_trade_entry_incarnation(handle, 0) == 1);
    CHECK(strategy_closed_trade_entry_incarnation(handle, 1) == 0);
    std::printf("H01 %s: entry=%lld provenance=%llu\n", name,
                static_cast<long long>(row.entry_time),
                static_cast<unsigned long long>(row.entry_incarnation));
}

} // namespace

int main() {
    // corpus/composite-boscurv-integration-01, Entry long 2025-03-31 12:30.
    check_h01_tape("boscurv", 1743423300000LL, "L", true);
    // corpus/composite-bracket-cap-range-pending-stop-01, Entry short 07:30.
    check_h01_tape("bracket", 1743405300000LL, "ShortOnGap", false);
    // corpus/composite-kanuck-calc-on-every-tick-01, Entry long 2025-04-01 02:15.
    check_h01_tape("kanuck", 1743472800000LL, "L", true);
    std::printf("L4e H01 provenance twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
