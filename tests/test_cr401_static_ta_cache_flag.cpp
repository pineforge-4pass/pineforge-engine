#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <limits>

namespace {

// Keep the oracle independent of the policy helper, including on the old head.
#if defined(PINEFORGE_STATIC_TA_CACHE) && PINEFORGE_STATIC_TA_CACHE == 0
constexpr bool expected_batch_flag = false;
#else
constexpr bool expected_batch_flag = true;
#endif

int failures = 0;

void check(bool passed, const char* expression, int line, const char* mode) {
    if (!passed) {
        std::fprintf(stderr, "%s:%d [%s]: %s\n", __FILE__, line, mode, expression);
        ++failures;
    }
}

// Explicit checks execute in Release even when NDEBUG is defined.
#define CHECK(expression) check((expression), #expression, __LINE__, mode)

using pineforge::Bar;

class CacheFlagHost final : public pineforge::source::PineStrategyHost {
public:
    CacheFlagHost() {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_pointvalue(1.0);
    }

    // closed_trade_equity() is protected on the host; expose it to the checks.
    double equity() const { return closed_trade_equity(); }

    int preparations = 0;
    int prepared_count = -1;
    bool allowed_precalculation = true;
    int callbacks = 0;

    void prepare_script_run(const Bar*, int n, bool allow_precalculation) override {
        ++preparations;
        prepared_count = n;
        allowed_precalculation = allow_precalculation;
        callbacks = 0;
    }

    void on_source_bar(const Bar&) override {
        ++callbacks;
        const int index = pine_bar_index();
        if (index == 0 || index == 3) {
            const double missing = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("L", true, missing, missing, 1.0);
        }
        if (index == 1 || index == 4) strategy_close_all();
    }
};

constexpr Bar bars[] = {
    {10.0, 10.0, 10.0, 10.0, 1.0,  60000},
    {11.0, 11.0, 11.0, 11.0, 1.0, 120000},
    {12.0, 12.0, 12.0, 12.0, 1.0, 180000},
    {13.0, 13.0, 13.0, 13.0, 1.0, 240000},
    {14.0, 14.0, 14.0, 14.0, 1.0, 300000},
    {15.0, 15.0, 15.0, 15.0, 1.0, 360000},
};

void check_run(const CacheFlagHost& host, bool expected_flag, const char* mode) {
    CHECK(host.last_error().empty());
    CHECK(host.preparations == 1);
    CHECK(host.prepared_count == 6);
    CHECK(host.allowed_precalculation == expected_flag);
    CHECK(host.callbacks == 6);
    CHECK(host.trade_count() == 2);
    CHECK(host.live_position_size() == 0.0);
    // Orders placed at closes 0/1 and 3/4 fill at the following opens:
    // 11 -> 12 and 14 -> 15, one unit each, so 1000 + 1 + 1 = 1002.
    CHECK(host.equity() == 1002.0);
    if (host.trade_count() == 2) {
        const auto first = host.get_trade(0);
        const auto second = host.get_trade(1);
        CHECK(first.is_long && first.qty == 1.0);
        CHECK(first.entry_price == 11.0 && first.exit_price == 12.0);
        CHECK(first.pnl == 1.0);
        CHECK(second.is_long && second.qty == 1.0);
        CHECK(second.entry_price == 14.0 && second.exit_price == 15.0);
        CHECK(second.pnl == 1.0);
    }
}

}  // namespace

int main() {
    CacheFlagHost batch;
    batch.run(bars, 6);
    check_run(batch, expected_batch_flag, "batch");

    // The explicit empty-timeframe overload is also statically eligible.
    CacheFlagHost empty_timeframes;
    empty_timeframes.run(bars, 6, "", "");
    check_run(empty_timeframes, expected_batch_flag, "batch-empty-timeframes");

    CacheFlagHost stream;
    const char* mode = "stream";
    const bool began = stream.stream_begin(bars, 6, "1", "1");
    CHECK(began);
    if (began) {
        check_run(stream, false, mode);
        CHECK(stream.stream_end());
        check_run(stream, false, mode);
    }

    CacheFlagHost magnified;
    magnified.run(bars, 6, "", "", true);
    check_run(magnified, false, "magnified");

    return failures == 0 ? 0 : 1;
}

#undef CHECK
