// L3a C-ABI stream witness: a generated-shaped source host must configure
// through prepare_native_begin before the public stream lifecycle begins.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>

namespace {

class StreamHost final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const pineforge::Bar&) override {}
};

int failures = 0;

void check(bool value, const char* operation, pf_strategy_t handle) {
    if (value) return;
    const char* error = strategy_get_last_error(handle);
    std::fprintf(stderr, "FAIL %s: %s\n", operation, error ? error : "<none>");
    ++failures;
}

} // namespace

int main() {
    StreamHost host;
    const auto handle = static_cast<pf_strategy_t>(&host);
    const pf_bar_t warmup{100.0, 100.0, 100.0, 100.0, 1.0, 0};
    check(strategy_stream_begin(handle, &warmup, 1, "1", "1") == 0,
          "strategy_stream_begin", handle);

    const std::int64_t fx_time[] = {0};
    const double fx_rate[] = {1.001};
    check(strategy_set_account_currency_fx_series(handle, fx_time, fx_rate, 1) == -1,
          "post-begin FX staging refusal", handle);

    const pf_trade_tick_t tick{60010, 7, 101.0, 0.5};
    check(strategy_stream_push_tick(handle, &tick) == 0,
          "strategy_stream_push_tick", handle);
    check(strategy_stream_advance_time(handle, 120000) == 0,
          "strategy_stream_advance_time", handle);
    check(strategy_stream_end(handle, 0) == 0, "strategy_stream_end", handle);

    pf_report_t report{};
    check(strategy_stream_fill_report(handle, &report) == 0,
          "strategy_stream_fill_report", handle);
    check(report.input_bars_processed == 2 && report.script_bars_processed == 2,
          "stream report counters", handle);
    pineforge::BacktestEngine::free_report(
        reinterpret_cast<pineforge::ReportC*>(&report));
    return failures == 0 ? 0 : 1;
}
