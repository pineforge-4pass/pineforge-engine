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

// Seeded positive for scripts/live_flags_lane.py: a strategy whose trading
// genuinely depends on pine_last_bar_index() (spec section 10.1's target).
// Enters long 5 bars before what it believes is the last bar, closes 2 bars
// later. Under set_realtime_tail(true, 2N), pine_last_bar_index() is frozen
// at 2N-1 for the ENTIRE run (not just the tail bar), so the trigger
// condition (bar_index == last_bar_index - 5 == 2N-6) is never satisfied
// within the fed [0, N-1] range: the entry never happens.
class LastBarDependentStrategy final : public pineforge::source::PineStrategyHost {
public:
    int entry_bar = -1;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == pine_last_bar_index() - 5) {
            strategy_entry("L", true);
            entry_bar = bar_index_;
        }
        if (entry_bar >= 0 && bar_index_ == entry_bar + 2) {
            strategy_close_all();
        }
    }
};

// Negative control: a modulo-bar_index entry/exit schedule (same shape as
// tests/test_live_flags_off_identity.cpp's Sma) that never reads
// last_bar_index/barstate.islast -- must be unaffected by realtime_tail
// except on the final bar (the harness's own range-end-close convention,
// scripts/live_flags_lane.py's `open_at_end_trade` exclusion; irrelevant at
// this engine-level pin since fill_report's trades_len already excludes it
// unless a position happens to still be open, which this schedule avoids by
// closing everything at bar_index % 11 == 9 well inside the feed).
class Indifferent final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar& b) override {
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
    const int N = 400;
    const auto bars = synth(N);

    // --- Positive: trading depends on pine_last_bar_index() ---
    LastBarDependentStrategy a;
    a.set_broker_state_hash_recording(true);
    a.run(bars.data(), N);
    ReportC ra{};
    a.fill_report(&ra);
    CHECK(ra.trades_len >= 1);                          // (1) flags-off: the trigger bar (N-6) is inside [0, N-1]
    CHECK(a.entry_bar == N - 6);

    LastBarDependentStrategy b;
    b.set_broker_state_hash_recording(true);
    b.set_realtime_tail(true, 2 * N);
    b.run(bars.data(), N);
    ReportC rb{};
    b.fill_report(&rb);
    CHECK(rb.trades_len == 0);                          // (2) trigger bar (2N-6) never comes within [0, N-1]
    CHECK(b.entry_bar == -1);

    CHECK(ra.broker_state_hash_len == N);
    CHECK(rb.broker_state_hash_len == N);
    if (ra.broker_state_hash_len == N && rb.broker_state_hash_len == N) {
        const int entry_bar = a.entry_bar;
        CHECK(entry_bar >= 0 && entry_bar < N - 1);     // interior, not the final bar
        for (int i = 0; i < entry_bar; ++i) {
            CHECK(ra.broker_state_hash[i] == rb.broker_state_hash[i]);   // (3a) equal before the entry bar
        }
        for (int i = entry_bar; i < N; ++i) {
            CHECK(ra.broker_state_hash[i] != rb.broker_state_hash[i]);   // (3b) differ from the entry bar onward
        }
    }
    BacktestEngine::free_report(&ra);
    BacktestEngine::free_report(&rb);

    // --- Negative control: a script that ignores last_bar_index sees no
    //     effect from realtime_tail on any interior bar. ---
    Indifferent c;
    c.set_broker_state_hash_recording(true);
    c.run(bars.data(), N);
    Indifferent d;
    d.set_broker_state_hash_recording(true);
    d.set_realtime_tail(true, 2 * N);
    d.run(bars.data(), N);
    CHECK(c.report_trade_count() >= 1);                 // non-vacuity: the control really does trade
    CHECK(same_trades(c, d));                           // (4) identical trades either way
    ReportC rc{};
    c.fill_report(&rc);
    ReportC rd{};
    d.fill_report(&rd);
    CHECK(rc.broker_state_hash_len == N && rd.broker_state_hash_len == N);
    if (rc.broker_state_hash_len == N && rd.broker_state_hash_len == N) {
        // Prefix only: the final bar's hash may differ (the harness's
        // range-end-close convention is skipped on the tail bar), so this
        // asserts [0, N-2], mirroring scripts/live_flags_lane.py's own
        // hash_first_diff() exclusion of each side's own last bar.
        for (int i = 0; i + 1 < N; ++i) CHECK(rc.broker_state_hash[i] == rd.broker_state_hash[i]);
    }
    BacktestEngine::free_report(&rc);
    BacktestEngine::free_report(&rd);

    return failures == 0 ? 0 : 1;
}
