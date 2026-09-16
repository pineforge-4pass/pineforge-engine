// A41(3) / L9c: after the first realtime tick, a C-ABI FX setter on a
// source-route handle must return -1 without latching UnsupportedSource.
// Legacy literals are the ab9714be output of the Fable delta-3 FX probe.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);             \
    }                                                                           \
} while (false)

class ProbeEngine final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}
};

struct Scenario {
    const char* tag;
    bool setter_after_tick;
    bool setter_in_warmup;
    int begin;
    int set_warmup;
    int tick1;
    int set_realtime;
    int tick2;
    int adv;
    int rep;
    int input_bars;
    int script_bars;
    int end;
    const char* err;
};

void scenario(const Scenario& expected) {
    ProbeEngine engine;
    pf_strategy_t handle = static_cast<pf_strategy_t>(&engine);
    pf_bar_t warmup{};
    warmup.open = warmup.high = warmup.low = warmup.close = 100.0;
    warmup.volume = 2.0;
    warmup.timestamp = 0;
    const int r_begin = strategy_stream_begin(handle, &warmup, 1, "1", "1");
    const std::int64_t ts[] = {0};
    const double rates[] = {1.001};
    const int r_set_w = expected.setter_in_warmup
        ? strategy_set_account_currency_fx_series(handle, ts, rates, 1) : 99;
    pf_trade_tick_t tick{};
    tick.timestamp = 60010;
    tick.sequence = 7;
    tick.price = 101.0;
    tick.quantity = 0.5;
    const int r_tick1 = strategy_stream_push_tick(handle, &tick);
    const int r_set_r = expected.setter_after_tick
        ? strategy_set_account_currency_fx_series(handle, ts, rates, 1) : 99;
    tick.timestamp = 60020;
    tick.sequence = 8;
    const int r_tick2 = strategy_stream_push_tick(handle, &tick);
    const int r_adv = strategy_stream_advance_time(handle, 120000);
    pf_report_t report{};
    const int r_rep = strategy_stream_fill_report(handle, &report);
    const int input_bars = report.input_bars_processed;
    const int script_bars = report.script_bars_processed;
    BacktestEngine::free_report(reinterpret_cast<ReportC*>(&report));
    const int r_end = strategy_stream_end(handle, 0);
    CHECK(r_begin == expected.begin);
    CHECK(r_set_w == expected.set_warmup);
    CHECK(r_tick1 == expected.tick1);
    CHECK(r_set_r == expected.set_realtime);
    CHECK(r_tick2 == expected.tick2);
    CHECK(r_adv == expected.adv);
    CHECK(r_rep == expected.rep);
    CHECK(input_bars == expected.input_bars);
    CHECK(script_bars == expected.script_bars);
    CHECK(r_end == expected.end);
    CHECK(engine.last_error() == expected.err);
    if (failures != 0) {
        std::printf("%-28s begin=%d set_warmup=%d tick1=%d set_realtime=%d "
                    "tick2=%d adv=%d rep=%d bars=%d/%d end=%d err='%s'\n",
                    expected.tag, r_begin, r_set_w, r_tick1, r_set_r, r_tick2,
                    r_adv, r_rep, input_bars, script_bars, r_end,
                    engine.last_error().c_str());
    }
}

}  // namespace

int main() {
    // ab9714be probe_base output (Fable delta-3 fx/probe.cpp).
    scenario({"no setter", false, false, 0, 99, 0, 99, 0, 0, 0, 2, 2, 0, ""});
    scenario({"setter in warmup", false, true, 0, -1, 0, 99, 0, 0, 0, 2, 2, 0, ""});
    scenario({"setter after tick", true, false, 0, 99, 0, -1, 0, 0, 0, 2, 2, 0, ""});
    scenario({"both", true, true, 0, -1, 0, -1, 0, 0, 0, 2, 2, 0, ""});
    std::printf("test_l9c_c_abi_fx_setter_after_realtime: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
