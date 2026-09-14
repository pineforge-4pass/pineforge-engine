#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
Bar bar(double o, double h, double l, double c, int64_t ts) { return Bar{o, h, l, c, 1.0, ts}; }
class Sma final : public pineforge::source::PineStrategyHost {   // a modulo-bar_index entry/exit schedule with a bracket, exercises fills
public:
    void on_source_bar(const Bar& b) override {
        // strategy_exit's signature is (id, from_entry, limit_price, stop_price, ...) --
        // limit BEFORE stop. A bracket exit with a stop 2% below close and a
        // limit 3% above close is therefore limit=close*1.03, stop=close*0.98.
        if (bar_index_ % 7 == 3) { strategy_entry("L", true); strategy_exit("x", "L", b.close * 1.03, b.close * 0.98); }
        if (bar_index_ % 11 == 9) strategy_close_all();
    }
};
std::vector<Bar> synth(int n) {
    std::vector<Bar> v; double p = 100;
    for (int i = 0; i < n; ++i) { const double d = std::sin(i * 0.37) * 2.0; v.push_back(bar(p, p + std::fabs(d) + 0.5, p - std::fabs(d) - 0.5, p + d, i * 60'000LL)); p += d; }
    return v;
}
bool same_trades(const BacktestEngine& a, const BacktestEngine& b) {
    if (a.report_trade_count() != b.report_trade_count()) return false;
    for (int i = 0; i < a.report_trade_count(); ++i) {
        const Trade& x = a.get_report_trade(i); const Trade& y = b.get_report_trade(i);
        if (x.entry_time != y.entry_time || x.exit_time != y.exit_time || x.entry_price != y.entry_price
            || x.exit_price != y.exit_price || x.qty != y.qty || x.pnl != y.pnl) return false;
    }
    return true;
}
}
int main() {
    const auto bars = synth(400);
    Sma fresh; fresh.run(bars.data(), 400);
    CHECK(fresh.report_trade_count() > 0);
    Sma reused; reused.run(bars.data(), 50);
    CHECK(reused.report_trade_count() > 0);
    reused.run(bars.data(), 400);      // reused == fresh
    CHECK(same_trades(fresh, reused));
    Sma flagged;                                                                  // every flag toggled on then off
    flagged.set_realtime_tail(true, 10); flagged.set_realtime_tail(false, 0);
    flagged.set_probe_suppress_tail_logic(true); flagged.set_probe_suppress_tail_logic(false);
    flagged.set_path_order(2); flagged.set_path_order(0);
    flagged.set_broker_state_hash_recording(true); flagged.set_broker_state_hash_recording(false);
    flagged.request_abort();                                                      // idle: no-op
    flagged.run(bars.data(), 400);
    CHECK(flagged.last_run_status() == 0);
    CHECK(same_trades(fresh, flagged));
    return failures == 0 ? 0 : 1;
}
