// Recording changes nothing a strategy run computes. Three trading hosts run
// the same orders over the same bars: one that declares no outputs, one that
// declares them and leaves recording off, and one that records. Against the
// first: the closed trades field for field, the equity curve, the per-bar
// broker-state hash of a batch, and in a stream the stream-state hash and the
// order actions after every input.
//
// Source-free: this TU runs in the kernel-only profile.
#include "outputs_test_support.hpp"

#include <memory>

using namespace outputs_test;

namespace {

constexpr std::int64_t kStep = 5 * kMinute;

enum class Mode { Undeclared, DeclaredOff, Recording };

std::unique_ptr<OutputsHost> trading_host(Mode mode) {
    auto host = std::make_unique<OutputsHost>(mode == Mode::Undeclared
        ? OutputsHost::Shape{0, 0, 0, false} : OutputsHost::Shape{2, 2, 1});
    // Every host makes the same recorder calls; only the third records.
    host->script = [](OutputsHost& h, const Bar& b, const NativeDecisionContext&) {
        const int k = h.published;
        if (k % 5 == 1) {
            h.submit_market({order_action::Transact{k % 2 == 0 ? 1.0 : -1.0}, "open", ""});
            h.event(0, b.close, "open");
        } else if (k % 5 == 4) {
            h.submit_market({execution::Flatten{}, "flat", ""});
            h.event(1, b.close);
        }
        h.value(0, b.close);
        h.value(1, strategy_current_equity(h.handle()));
        h.constant(0, 3.0);
    };
    if (mode == Mode::Recording) CHECK(strategy_outputs_set_enabled(host->handle(), 1) == 0);
    return host;
}

// The kernel records the equity curve and, with recording on, the per-bar
// broker-state hash into the report.
NativeRunSpec reporting_spec(const char* key) {
    NativeRunSpec spec = make_spec(key, 1);
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    return spec;
}

struct Report {
    std::vector<pf_trade_t> trades;
    std::vector<pf_equity_point_t> equity;
    std::vector<std::uint64_t> hashes;
    std::int64_t processed = 0;
    double net_profit = 0.0;
};

Report report_of(pf_strategy_t s) {
    Report out;
    pf_report_t rep;
    std::memset(&rep, 0, sizeof(rep));
    CHECK(strategy_stream_fill_report(s, &rep) == 0);
    out.trades.assign(rep.trades, rep.trades + rep.trades_len);
    out.equity.assign(rep.equity_curve, rep.equity_curve + rep.equity_curve_len);
    out.hashes.assign(rep.broker_state_hash, rep.broker_state_hash + rep.broker_state_hash_len);
    out.processed = rep.script_bars_processed;
    out.net_profit = rep.net_profit;
    BacktestEngine::free_report(reinterpret_cast<ReportC*>(&rep));
    return out;
}

bool same_trade(const pf_trade_t& a, const pf_trade_t& b) {
    return a.entry_time == b.entry_time && a.exit_time == b.exit_time
        && same_value(a.entry_price, b.entry_price) && same_value(a.exit_price, b.exit_price)
        && same_value(a.pnl, b.pnl) && same_value(a.pnl_pct, b.pnl_pct) && a.is_long == b.is_long
        && same_value(a.max_runup, b.max_runup) && same_value(a.max_drawdown, b.max_drawdown)
        && same_value(a.qty, b.qty) && same_value(a.commission, b.commission)
        && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
        && a.open_at_end == b.open_at_end;
}

bool same_report(const Report& a, const Report& b) {
    if (a.trades.size() != b.trades.size() || a.equity.size() != b.equity.size()) return false;
    for (std::size_t i = 0; i < a.trades.size(); ++i)
        if (!same_trade(a.trades[i], b.trades[i])) return false;
    for (std::size_t i = 0; i < a.equity.size(); ++i) {
        if (a.equity[i].time_ms != b.equity[i].time_ms
            || !same_value(a.equity[i].equity, b.equity[i].equity)
            || !same_value(a.equity[i].open_profit, b.equity[i].open_profit)) return false;
    }
    return a.hashes == b.hashes && a.processed == b.processed
        && same_value(a.net_profit, b.net_profit);
}

void batch() {
    const auto bars = make_bars(40, kStep);
    Report reports[3];
    int k = 0;
    for (Mode mode : {Mode::Undeclared, Mode::DeclaredOff, Mode::Recording}) {
        auto host = trading_host(mode);
        pf_strategy_t s = host->handle();
        strategy_set_broker_state_hash_recording(s, 1);
        CHECK(host->configure_native(reporting_spec("outputs-off-identity")).status
              == NativeSetupStatus::Applied);
        host->run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host->native_state().kind == NativeLifecycleKind::Completed);
        reports[k] = report_of(s);
        CHECK(strategy_outputs_bars_len(s) == (mode == Mode::Recording ? 40 : 0));
        ++k;
    }
    CHECK(reports[0].trades.size() >= 6);
    CHECK(reports[0].hashes.size() == 40);
    CHECK(reports[0].equity.size() == 40);
    CHECK(same_report(reports[0], reports[1]));
    CHECK(same_report(reports[0], reports[2]));
}

struct StreamTrace {
    std::vector<std::uint64_t> state_hashes;   // after begin and after every push
    std::vector<int> actions;
    Report report;
};

void stream() {
    const auto bars = make_bars(36, kStep);
    std::vector<pf_bar_t> c_bars;
    for (const Bar& b : bars) c_bars.push_back({b.open, b.high, b.low, b.close, b.volume, b.timestamp});
    StreamTrace traces[3];
    int k = 0;
    for (Mode mode : {Mode::Undeclared, Mode::DeclaredOff, Mode::Recording}) {
        auto host = trading_host(mode);
        pf_strategy_t s = host->handle();
        CHECK(host->configure_native(reporting_spec("outputs-off-identity-stream")).status
              == NativeSetupStatus::Applied);
        CHECK(strategy_stream_begin(s, c_bars.data(), 12, "", "") == 0);
        StreamTrace& trace = traces[k++];
        trace.state_hashes.push_back(strategy_stream_state_hash(s));
        trace.actions.push_back(strategy_stream_order_actions_len(s));
        for (std::size_t i = 12; i < c_bars.size(); ++i) {
            CHECK(strategy_stream_push_bar(s, &c_bars[i]) == 0);
            trace.state_hashes.push_back(strategy_stream_state_hash(s));
            trace.actions.push_back(strategy_stream_order_actions_len(s));
            strategy_stream_order_actions_clear(s);
            strategy_outputs_events_clear(s);
        }
        CHECK(strategy_stream_end(s, 0) == 0);
        trace.report = report_of(s);
        CHECK(strategy_outputs_bars_len(s) == (mode == Mode::Recording ? 36 : 0));
    }
    CHECK(traces[0].state_hashes.size() == 25);
    CHECK(traces[0].report.trades.size() >= 5);
    CHECK(traces[0].report.equity.size() == 36);
    for (int i = 1; i < 3; ++i) {
        CHECK(traces[i].state_hashes == traces[0].state_hashes);
        CHECK(traces[i].actions == traces[0].actions);
        CHECK(same_report(traces[i].report, traces[0].report));
    }
}

}  // namespace

int main() {
    batch();
    stream();
    return finish("test_outputs_off_identity");
}
