// Base-compatible test strategy module AND non-vacuity guard executable.
// Red feature assertions live in the Python rows; this guard must pass today.
#include "replay_rq_fixture.hpp"
#include <pineforge/run_failure.hpp>

#include <cstdio>

namespace {
replay_rq::Host& fixture(pf_strategy_t state) {
    return *static_cast<replay_rq::Host*>(static_cast<pineforge::BacktestEngine*>(state));
}
}

extern "C" {
// Only the sealed shape opts into retirement. Its strategy never indexes
// retired rows; sort control reads the retained tail before publication.
PF_API int strategy_reads_trade_history() { return 0; }
pf_strategy_t strategy_create(const char*) {
    return static_cast<pineforge::BacktestEngine*>(new replay_rq::Host);
}
void strategy_free(pf_strategy_t state) { delete &fixture(state); }
void strategy_set_input(pf_strategy_t state, const char* key, const char* value) {
    if (std::strcmp(key, "shape") == 0) {
        fixture(state).shape = value;
        fixture(state).configure();
    }
}
void strategy_set_override(pf_strategy_t state, const char* key, const char* value) {
    replay_rq::require(std::strcmp(key, "initial_capital") == 0, "unknown fixture override");
    pineforge::source::StrategyOverrides overrides;
    overrides.initial_capital = std::stod(value);
    fixture(state).set_strategy_override(overrides);
}
void run_backtest_full(pf_strategy_t state, pf_bar_t* bars, int count,
                       const char* input_tf, const char* script_tf, int magnifier,
                       int samples, pf_magnifier_distribution_t distribution, pf_report_t* out) {
    *out = {};
    try {
        std::vector<pineforge::Bar> values;
        for (int i = 0; i < count; ++i)
            values.push_back({bars[i].open, bars[i].high, bars[i].low, bars[i].close,
                              bars[i].volume, bars[i].timestamp});
        replay_rq::run(fixture(state), values, input_tf, script_tf, magnifier != 0,
                     samples, static_cast<pineforge::MagnifierDistribution>(distribution));
        fixture(state).fill_report(reinterpret_cast<pineforge::ReportC*>(out));
    } catch (const std::exception& error) {
        pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(out));
        *out = {};
        pineforge::note_run_failure(fixture(state), "replay R/Q fixture", error);
    }
}
void run_backtest(pf_strategy_t state, pf_bar_t* bars, int count, pf_report_t* out) {
    run_backtest_full(state, bars, count, "1", "1", 0, 4, PF_MAGNIFIER_ENDPOINTS, out);
}
void report_free(pf_report_t* report) {
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
}
// Test-only observations of existing state, not replay feature accessors.
PF_API std::uint64_t replay_rq_observe(pf_strategy_t state, int key, int group) {
    const auto& host = fixture(state);
    switch (key) {
    case 0: return host.groups;
    case 1: return host.parents.at(static_cast<std::size_t>(group));
    case 2: return host.group_first.at(static_cast<std::size_t>(group));
    case 3: return host.group_end.at(static_cast<std::size_t>(group));
    case 4: return host.physical_count.at(static_cast<std::size_t>(group));
    case 5: return host.ledger_base();
    case 6: return host.prefix_base();
    case 7: return host.sort_first;
    default: throw std::runtime_error("unknown fixture observation");
    }
}
}

#ifndef PINEFORGE_REPLAY_RQ_MODULE
namespace {
void check_global_shape(const std::string& shape, const replay_rq::Host& host,
                        const pf_report_t& report) {
    replay_rq::require(host.last_run_status() == 0 && host.last_error().empty(),
                      "fixture did not complete");
    replay_rq::require(host.groups == (shape == "two_groups" ? 2u : 1u),
                      "fixture did not reach global replacement");
    replay_rq::require(report.trades_len == (shape == "any" ? 2 : shape == "two_groups" ? 6 : 3),
                      "fixture did not produce the specified replacement rows");
    for (std::size_t group = 0; group < host.groups; ++group)
        replay_rq::require(host.physical_count[group] == (shape == "cancellation" ? 3u : 2u),
                          "group lacks its specified physical closes");
    if (shape == "any")
        replay_rq::require(host.row(0).entry_id == "A" && host.row(1).entry_id == "Z",
                          "ANY fixture did not actually permute its source rows");
    if (shape == "two_groups") {
        replay_rq::require(host.parents[0] != host.parents[1], "groups share an event");
        replay_rq::require(report.trades[0].exit_bar_index == report.trades[3].exit_bar_index,
                          "groups did not occur in the same bar");
        replay_rq::require(host.group_first[0] == 0 && host.group_end[0] == 3
                          && host.group_first[1] == 3 && host.group_end[1] == 6,
                          "groups lack separate three-row FIFO replacements");
        for (int i = 0; i < report.trades_len; ++i)
            replay_rq::require(report.trades[i].exit_bar_index == 3,
                              "a group row left the specified exit bar");
    }
    const double large = static_cast<double>(UINT64_C(1) << 54);
    const double cancellation = ((0.0 + ((large - 1.0) - (0.5 + 0.5)))
        + ((large - large) - (0.5 + 0.5)))
        + ((large - (large * 2.0)) - (0.5 + 0.5));
    const double wanted = shape == "cancellation" ? cancellation
        : shape == "fifo_no_fees" ? (136.0 - 128.0) * (4.0 + 2.0)
        : shape == "sort" ? ((136.0 - 128.0) - 2.0) * (1.0 + 4.0)
        : ((136.0 - 128.0) - 2.0) * (4.0 + 2.0) * (shape == "two_groups" ? 2.0 : 1.0);
    const double actual = host.raw_sum_before_extension();
    replay_rq::require(std::memcmp(&actual, &wanted, sizeof actual) == 0,
                      "physical-close scenario arithmetic differs");
    if (shape == "cancellation") {
        double presented = 0.0;
        for (int i = 0; i < report.trades_len; ++i) presented += report.trades[i].pnl;
        replay_rq::require(std::memcmp(&presented, &actual, sizeof actual) != 0,
                          "cancellation vector does not distinguish report order");
    }
}

void check_terminal_shape(const std::string& shape, const replay_rq::Host& host,
                          const pf_report_t& report) {
    // Ordinary/range-end/terminal-quote eligibility is independent of replay.
    replay_rq::require(host.last_run_status() == 0 && host.last_error().empty(),
                      "terminal fixture did not complete");
    replay_rq::require(report.trades_len == 2 && report.trades[0].open_at_end == 0
                      && report.trades[1].open_at_end != 0,
                      "terminal fixture lacks one physical close and one held lot");
    const double expected_closed = ((shape == "quote" ? 128.0 : 136.0) - 128.0)
        - (1.0 + 1.0);
    const double actual_closed = host.raw_sum_before_extension();
    replay_rq::require(std::memcmp(&actual_closed, &expected_closed,
                                 sizeof actual_closed) == 0,
                      "terminal fixture booked the wrong physical P&L");
    const double expected_terminal = (144.0 - 128.0) - (1.0 + 1.0);
    replay_rq::require(std::memcmp(&report.trades[1].pnl, &expected_terminal,
                                 sizeof expected_terminal) == 0,
                      "held-lot report operand is not the fixed terminal mark");
    replay_rq::require(report.equity_curve_len == (shape == "quote" ? 4 : 8),
                      "terminal fixture has the wrong script-point population");
    if (shape == "quote") {
        replay_rq::require(host.report_terminal_quote_applied(
            *reinterpret_cast<const pineforge::ReportC*>(&report)),
            "terminal quote was legitimately ineligible");
        replay_rq::require(report.equity_curve[3].time_ms == replay_rq::start
                          + 60 * replay_rq::minute,
                          "terminal quote did not retime the completed minute-45 mark");
    }
}

void check_nondyadic_shape(const replay_rq::Host& host, const pf_report_t& report) {
    replay_rq::require(host.last_run_status() == 0 && host.last_error().empty(),
                      "decimal-price fixture did not complete");
    replay_rq::require(host.groups == 0 && report.trades_len == 4,
                      "decimal-price fixture needs three separate closes and one held lot");
    const std::array<double, 4> entries{100.7, 101.7, 102.9, 103.7};
    const std::array<double, 4> exits{100.9, 101.9, 102.7, 104.9};
    const std::array<double, 4> quantities{3.0, 5.0, 13.0, 1.0};
    const std::array<int, 4> entry_bars{1, 3, 5, 7}, exit_bars{2, 4, 6, 8};
    double expected_net = +0.0, presented_net = +0.0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& row = report.trades[i];
        replay_rq::require(row.is_long != 0 && (row.open_at_end != 0) == (i == 3),
                          "decimal-price fixture has the wrong close or terminal population");
        replay_rq::require(row.entry_bar_index == entry_bars[i]
                          && row.exit_bar_index == exit_bars[i]
                          && row.entry_time == replay_rq::start + entry_bars[i] * replay_rq::minute
                          && row.exit_time == replay_rq::start + exit_bars[i] * replay_rq::minute,
                          "decimal-price orders did not fill on the scheduled bars");
        replay_rq::require(std::memcmp(&row.entry_price, &entries[i], sizeof(double)) == 0
                          && std::memcmp(&row.exit_price, &exits[i], sizeof(double)) == 0
                          && std::memcmp(&row.qty, &quantities[i], sizeof(double)) == 0,
                          "decimal-price fills differ from the independent schedule");
        const double entry_fee = 0.07 * quantities[i];
        const double exit_fee = 0.07 * quantities[i];
        const double commission = entry_fee + exit_fee;
        double pnl = (exits[i] - entries[i]) * quantities[i] * 1.0 * 1.0;
        pnl -= entry_fee + exit_fee;
        replay_rq::require(std::memcmp(&row.pnl, &pnl, sizeof(double)) == 0
                          && std::memcmp(&row.commission, &commission, sizeof(double)) == 0,
                          "decimal-price row changed its binary64 close operands");
        if (i < 3) {
            expected_net = expected_net + pnl;
            presented_net = presented_net + row.pnl;
        }
    }
    const double actual_net = host.raw_sum_before_extension();
    replay_rq::require(std::memcmp(&actual_net, &expected_net, sizeof(double)) == 0
                      && std::memcmp(&actual_net, &presented_net, sizeof(double)) == 0,
                      "decimal-price presented close fold differs from the booked total");
    replay_rq::require(report.equity_curve_len == 9,
                      "decimal-price fixture has the wrong equity-point population");
}

std::string single_line(std::string text) {
    for (char& value : text)
        if (value == '\n' || value == '\r') value = ' ';
    return text;
}
}  // namespace

int main() {
    std::string failed_shapes;
    // Each shape owns a fresh run. A failed shape is retained as a failure,
    // while the later guards still execute and print their own observations.
    for (const std::string shape : {
            "any", "fifo", "fifo_no_fees", "sort", "two_groups", "cancellation",
            "ordinary", "quote", "nondyadic"}) {
        pf_strategy_t state = nullptr;
        pf_report_t report{};
        std::string failure;
        bool passed = false;
        try {
            state = strategy_create("{}");
            strategy_set_input(state, "shape", shape.c_str());
            const auto bars = replay_rq::tape(shape);
            run_backtest_full(state, reinterpret_cast<pf_bar_t*>(
                const_cast<pineforge::Bar*>(bars.data())), static_cast<int>(bars.size()),
                "1", shape == "quote" ? "15" : "1", shape == "quote" ? 1 : 0,
                4, PF_MAGNIFIER_ENDPOINTS, &report);
            const auto& host = fixture(state);
            if (shape == "nondyadic")
                check_nondyadic_shape(host, report);
            else if (shape == "ordinary" || shape == "quote")
                check_terminal_shape(shape, host, report);
            else
                check_global_shape(shape, host, report);
            passed = true;
        } catch (const std::exception& error) {
            failure = error.what();
        } catch (...) {
            failure = "non-standard exception";
        }
        const auto* host = state ? &fixture(state) : nullptr;
        const auto error = single_line(host ? host->last_error() : "strategy_create failed");
        const auto assertion = single_line(failure);
        std::fprintf(stderr,
            "GUARD shape=%s status=%d error=\"%s\" trades=%d events=%zu "
            "physical=%zu,%zu parents=%llu,%llu rows=%zu:%zu,%zu:%zu "
            "points=%d raw=%.17g sort_first=%zu code=%s args=%s result=%s assertion=\"%s\"\n",
            shape.c_str(), host ? host->last_run_status() : -1, error.c_str(),
            report.trades_len, host ? host->groups : 0u,
            host ? host->physical_count[0] : 0u, host ? host->physical_count[1] : 0u,
            static_cast<unsigned long long>(host ? host->parents[0] : 0u),
            static_cast<unsigned long long>(host ? host->parents[1] : 0u),
            host ? host->group_first[0] : 0u, host ? host->group_end[0] : 0u,
            host ? host->group_first[1] : 0u, host ? host->group_end[1] : 0u,
            report.equity_curve_len, host ? host->raw_sum_before_extension() : replay_rq::absent,
            host ? host->sort_first : 0u,
            host ? pineforge::run_failure_code_of(*host) : "",
            host ? pineforge::run_failure_args_of(*host) : "{}",
            passed ? "pass" : "fail", assertion.c_str());
        report_free(&report);
        if (state) strategy_free(state);
        if (!passed) failed_shapes += " " + shape;
    }
    if (!failed_shapes.empty()) {
        std::fprintf(stderr, "FAIL replay R/Q fixture shapes:%s\n", failed_shapes.c_str());
        return 1;
    }
    std::puts("PASS replay R/Q production-path fixture guards (not replay implementation)");
    return 0;
}
#endif
