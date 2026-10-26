// CAGR is proportional below one day and on compounded overflow.
// Every remaining report overflow is R(nonfinite).
#include <pineforge/metrics.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include "../runner/report.hpp"
#include "equity_stats_reference.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
const char* row = "";
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kMax = std::numeric_limits<double>::max();
constexpr std::int64_t kYear = 31'557'600'000LL;
constexpr std::int64_t kDay = 86'400'000LL;
constexpr std::int64_t kTime = 1704067200000LL;

#define CHECK(expr) do { if (!(expr)) {                                      \
    std::fprintf(stderr, "FAIL [%s] %s:%d %s\n", row, __FILE__, __LINE__, #expr); \
    ++failures;                                                             \
} } while (0)

void nonfinite(const char* name, const std::string& field,
               const std::function<void()>& compute) {
    row = name;
    try {
        compute();
        CHECK(false);  // Returning (including NaN/null/zero) is the base defect.
    } catch (const std::exception& error) {
        const auto failure = classify_run_failure(error);
        CHECK(failure.code == RunFailureCode::report_result_invalid);
        CHECK(run_failure_code_class(failure.code) == RunFailureClass::engine_fault);
        CHECK(!run_failure_code_retryable(failure.code));
        CHECK(failure.args && *failure.args ==
              "{\"field\":\"" + field + "\",\"reason\":\"nonfinite\"}");
    }
}

bool same_bits(double left, double right) {
    std::uint64_t a = 0, b = 0;
    std::memcpy(&a, &left, sizeof a);
    std::memcpy(&b, &right, sizeof b);
    return a == b;
}

TradeC trade(double pnl, double pct = 1.0, double fee = 0.0) {
    TradeC t{};
    t.entry_price = 1.0;
    t.exit_price = 2.0;
    t.qty = 0.5;
    t.pnl = pnl;
    t.pnl_pct = pct;
    t.commission = fee;
    t.is_long = 1;
    t.entry_bar_index = 0;
    t.exit_bar_index = 1;
    return t;
}

pf_trade_stats_t stats(const std::vector<TradeC>& trades, double capital = 1e308,
                      metrics::TradeFilter filter = metrics::TradeFilter::ALL) {
    return metrics::compute_trade_stats(trades.data(), static_cast<int>(trades.size()),
                                        filter, capital);
}

// Inject producer rows directly: this deliberately bypasses settlement, whose
// earlier guards have independent coverage. Exercise the actual report copy
// and aggregate, including the range-end section, without strategy semantics.
class ReportRowsHost : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    void append(const TradeC& value, bool range_end = false) {
        Trade t{};
        t.entry_time = value.entry_time; t.exit_time = value.exit_time;
        t.entry_price = value.entry_price; t.exit_price = value.exit_price;
        t.pnl = value.pnl; t.pnl_pct = value.pnl_pct;
        t.qty = value.qty; t.commission = value.commission;
        t.is_long = value.is_long != 0;
        t.max_runup = value.max_runup; t.max_drawdown = value.max_drawdown;
        t.entry_bar_index = value.entry_bar_index; t.exit_bar_index = value.exit_bar_index;
        // The two report rows live in differently typed stores since the O(1)
        // contract: closed rows in TradeLedger, the range-end section in the
        // constant-memory OpenReportRowSource (R12). Seeding the latter is its
        // documented reference-oracle path.
        if (range_end) {
            range_end_trades_.push_back(t);
        } else {
            trades_.push_back(t);
        }
    }
    // This protected helper retains initialized allocations for its caller to
    // clean up. Public fill_report owns that storage and publishes none on fault.
    void read(ReportC& out) const { fill_trades_section(&out); }
};

void report_trade_assembly() {
    for (bool tail : {false, true}) {
        ReportRowsHost host;
        host.append(trade(1e308));
        host.append(trade(1e308), tail);
        nonfinite("report_net_profit_sum", "summary.net_pnl", [&] {
            ReportC out{};
            try { host.read(out); }
            catch (...) {
                CHECK(out.total_trades == 2 && out.trades_len == 2);
                CHECK(out.trades != nullptr);
                if (out.trades && out.trades_len == 2)
                    CHECK(out.trades[0].pnl == 1e308 && out.trades[1].pnl == 1e308);
                BacktestEngine::free_report(&out);
                throw;
            }
            BacktestEngine::free_report(&out);
        });
    }
    ReportRowsHost corrupted;
    auto bad = trade(1);
    bad.max_runup = kNaN;
    corrupted.append(bad, true);
    nonfinite("report_trade_invariant", "trades.max_runup", [&] {
        ReportC out{};
        try { corrupted.read(out); }
        catch (...) { BacktestEngine::free_report(&out); throw; }
        BacktestEngine::free_report(&out);
    });
    ReportRowsHost prefix;
    prefix.append(trade(3));
    prefix.append(bad, true);
    nonfinite("report_trade_invariant_keeps_initialized_prefix", "trades.max_runup", [&] {
        ReportC partial{};
        try {
            prefix.read(partial);
        } catch (...) {
            CHECK(partial.total_trades == 1 && partial.trades_len == 1);
            CHECK(partial.net_profit == 3);
            CHECK(partial.trades != nullptr);
            if (partial.trades && partial.trades_len == 1)
                CHECK(partial.trades[0].pnl == 3 && partial.trades[0].qty == 0.5);
            BacktestEngine::free_report(&partial);
            throw;
        }
        BacktestEngine::free_report(&partial);
    });
    row = "report_trade_assembly_finite";
    ReportRowsHost finite;
    finite.append(trade(3));
    finite.append(trade(-1), true);
    ReportC out{};
    finite.read(out);
    CHECK(out.trades_len == 2 && out.total_trades == 2 && out.net_profit == 2);
    CHECK(out.trades[0].pnl == 3 && out.trades[1].pnl == -1);
    BacktestEngine::free_report(&out);
}

void earlier_failure_keeps_its_code() {
    row = "earlier_failure_keeps_its_code";
    ReportRowsHost host;
    NativeRunSpec spec;
    spec.identity = {"metrics-earlier-failure", 1};
    spec.input_tf = "1"; spec.script_tf = "1";
    spec.tickerid = "TEST:METRICS";
    spec.timezone = "UTC"; spec.session = "24x7";
    spec.initial_capital = 1;
    spec.point_value = 1; spec.account_fx = 1;
    spec.price_tick = 1;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const Bar bar{1, 1, 1, 1, 1, kTime};
    host.run(&bar, 1);
    CHECK(host.last_error().empty());
    auto bad = trade(1);
    bad.max_runup = kInf;
    host.append(bad);  // Faulted public reads must not revisit this invalid row.
    note_run_failure(host, "earlier strategy stop", RunFailureCode::strategy_runtime_error);
    ReportC out{};
    for (int read = 0; read != 2; ++read) {
        host.fill_report(&out);
        CHECK(out.trades == nullptr && out.trades_len == 0 && out.total_trades == 0);
        CHECK(out.equity_curve == nullptr && out.equity_curve_len == 0);
        CHECK(out.security_diag == nullptr && out.security_diag_len == 0);
        CHECK(out.trace == nullptr && out.trace_len == 0);
        CHECK(out.trace_names == nullptr && out.trace_names_len == 0);
        CHECK(out.broker_state_hash == nullptr && out.broker_state_hash_len == 0);
        CHECK(out.net_profit == 0 && out.input_bars_processed == 0
              && out.script_bars_processed == 0);
        CHECK(out.metrics.all.num_trades == 0 && out.metrics.all.net_profit == 0);
        CHECK(run_failure_value_of(host).code == RunFailureCode::strategy_runtime_error);
        CHECK(host.last_error() == "earlier strategy stop");
        CHECK(std::string(run_failure_args_of(host)) == "{}");
        BacktestEngine::free_report(&out);
    }
}

void check_empty_public_report(const ReportC& out) {
    CHECK(out.trades == nullptr && out.trades_len == 0 && out.total_trades == 0);
    CHECK(out.equity_curve == nullptr && out.equity_curve_len == 0);
    CHECK(out.security_diag == nullptr && out.security_diag_len == 0);
    CHECK(out.trace == nullptr && out.trace_len == 0);
    CHECK(out.trace_names == nullptr && out.trace_names_len == 0);
    CHECK(out.broker_state_hash == nullptr && out.broker_state_hash_len == 0);
    CHECK(out.net_profit == 0 && out.input_bars_processed == 0 && out.script_bars_processed == 0);
    CHECK(out.security_feeds_total == 0 && out.security_eval_complete_total == 0
          && out.security_eval_partial_total == 0);
    CHECK(out.metrics.all.num_trades == 0 && out.metrics.all.net_profit == 0);
    CHECK(out.metrics.equity.buy_hold_return == 0 && out.metrics.equity.cagr == 0);
}

class AssemblyFaultHost final : public ReportRowsHost {
public:
    enum class Fault { None, Unknown, Recorded, RecordedThenThrow };
    Fault fault = Fault::None;
    mutable int presentations = 0;
    void present_report(ReportC*) const override {
        ++presentations;
        if (fault == Fault::Recorded || fault == Fault::RecordedThenThrow)
            note_run_failure(const_cast<AssemblyFaultHost&>(*this),
                "original presentation failure", RunFailureCode::strategy_runtime_error);
        if (fault == Fault::Unknown || fault == Fault::RecordedThenThrow) throw 73;
    }
};

void public_report_assembly_faults() {
    for (int mode = 0; mode != 4; ++mode) {
        row = "public_report_assembly_fault_after_successful_read";
        AssemblyFaultHost host;
        NativeRunSpec spec;
        spec.identity = {"report-assembly", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:ASSEMBLY";
        spec.timezone = "UTC"; spec.session = "24x7";
        spec.initial_capital = 10000;
        spec.point_value = spec.account_fx = 1;
        spec.price_tick = 1;
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const Bar bars[]{{100, 100, 100, 100, 1, kTime},
                         {100, 100, 100, 100, 1, kTime + 60000}};
        host.run(bars, 2);
        CHECK(host.last_run_status() == 0 && host.last_error().empty());
        host.append(trade(3));
        ReportC before{};
        host.fill_report(&before);
        CHECK(before.total_trades == 1 && before.trades_len == 1 && before.trades);
        CHECK(before.equity_curve_len == 2 && before.equity_curve);
        CHECK(host.presentations == 1);
        BacktestEngine::free_report(&before);

        if (mode == 0) {
            auto bad = trade(1);
            bad.max_runup = kInf;
            host.append(bad);
        } else {
            host.fault = mode == 1 ? AssemblyFaultHost::Fault::Unknown
                : mode == 2 ? AssemblyFaultHost::Fault::Recorded
                            : AssemblyFaultHost::Fault::RecordedThenThrow;
        }
        ReportC failed;
        std::memset(&failed, 0xA5, sizeof failed);
        bool threw = false;
        RunFailureValue carried{};
        try {
            host.fill_report(&failed);
        } catch (const std::exception& error) {
            threw = true;
            carried = classify_run_failure(error);
        } catch (...) {
            CHECK(false);  // Unknown assembly exceptions must become typed failures.
        }
        CHECK(threw == (mode != 2));
        check_empty_public_report(failed);
        const auto expected = mode == 0 ? RunFailureCode::report_result_invalid
            : mode == 1 ? RunFailureCode::engine_unclassified_error
                        : RunFailureCode::strategy_runtime_error;
        CHECK(host.last_run_status() == 1 && run_failure_value_of(host).code == expected);
        if (threw) CHECK(carried.code == expected);
        const std::string first_text = host.last_error();
        const std::string first_args = run_failure_args_of(host);
        if (mode >= 2) CHECK(first_text == "original presentation failure");
        const int presentations = host.presentations;
        for (int repeat = 0; repeat != 2; ++repeat) {
            host.fill_report(&failed);
            check_empty_public_report(failed);
            CHECK(host.presentations == presentations);
            CHECK(host.last_error() == first_text);
            CHECK(std::string(run_failure_args_of(host)) == first_args);
            CHECK(run_failure_value_of(host).code == expected);
            BacktestEngine::free_report(&failed);
        }
        BacktestEngine::free_report(&failed);
        // Fault suppression never rolls back the committed internal row space.
        CHECK(host.closed_trade_count() == (mode == 0 ? 2u : 1u));
    }
}

pf_equity_stats_t equity(std::vector<double> values, double capital = 1.0,
                         std::int64_t step = kYear, double net_profit = 0.0,
                         double first_open = 1.0, double last_close = 1.0) {
    std::vector<pf_equity_point_t> curve;
    for (std::size_t i = 0; i < values.size(); ++i)
        curve.push_back({kTime + static_cast<std::int64_t>(i) * step, values[i], 0.0});
    return metrics::compute_equity_stats(curve.data(), static_cast<std::int64_t>(curve.size()),
                                         capital, "UTC", first_open, last_close, 0, net_profit);
}

void trade_arithmetic() {
    struct Case {
        const char* name;
        const char* field;
        std::vector<TradeC> trades;
        double capital = 1e308;
    };
    const std::vector<Case> cases = {
        {"net_sum", "net_profit", {trade(1e308), trade(1e308)}},
        {"commission_sum", "commission_paid", {trade(1, 1, 1e308), trade(1, 1, 1e308)}},
        {"pct_sum", "avg_trade_pct", {trade(1, 1e308), trade(1, 1e308)}},
        {"gross_profit_sum", "gross_profit", {trade(1e308), trade(-1e308), trade(1e308)}},
        {"gross_loss_sum", "gross_loss", {trade(-1e308), trade(1e308), trade(-1e308)}},
        {"win_pct_sum", "avg_win_pct",
         {trade(1, 1e308), trade(-1, -1e308), trade(1, 1e308)}},
        {"loss_pct_sum", "avg_loss_pct",
         {trade(-1, -1e308), trade(1, 1e308), trade(-1, -1e308)}},
        {"net_pct_ratio", "net_profit_pct", {trade(1)}, 1e-308},
        {"net_pct_product", "net_profit_pct", {trade(1e308)}, 10},
        {"gross_profit_pct", "gross_profit_pct", {trade(1), trade(-1)}, 1e-308},
        {"gross_loss_pct", "gross_loss_pct", {trade(1), trade(-2)}, 1e-306},
        {"profit_factor", "profit_factor", {trade(1e308), trade(-1e-10)}},
        {"win_loss_ratio", "ratio_avg_win_avg_loss",
         {trade(1e308), trade(-0.5), trade(-0.5)}},
    };
    for (auto filter : {metrics::TradeFilter::ALL, metrics::TradeFilter::LONG,
                        metrics::TradeFilter::SHORT}) {
        const char* block = filter == metrics::TradeFilter::ALL ? "metrics.all." :
                            filter == metrics::TradeFilter::LONG ? "metrics.longs." :
                                                                  "metrics.shorts.";
        for (const auto& test : cases) {
            auto trades = test.trades;
            for (auto& t : trades) t.is_long = filter != metrics::TradeFilter::SHORT;
            nonfinite(test.name, std::string(block) + test.field,
                      [&] { (void)stats(trades, test.capital, filter); });
        }
    }
}

void equity_arithmetic() {
    nonfinite("c18_3_buy_hold_return", "metrics.equity.buy_hold_return", [] {
        // The executed account can stay finite: qty .5 * PV 1e300 *
        // (2.5e8 - 1) + capital 1e301 < DBL_MAX. Buy/hold uses all capital.
        (void)equity({1e301, 1e301}, 1e301, 7 * 60'000, 0, 1, 2.5e8);
    });
    nonfinite("buy_hold_ratio", "metrics.equity.buy_hold_return_pct", [] {
        (void)equity({}, 1, kYear, 0, 1e-308, 1e308);
    });
    nonfinite("buy_hold_pct_product", "metrics.equity.buy_hold_return_pct", [] {
        (void)equity({}, 1, kYear, 0, 1, 1e307);
    });
    nonfinite("cagr_pow_and_simple_overflow", "metrics.equity.cagr", [] {
        const double span_years = static_cast<double>(kDay) / (365.25 * 86400.0 * 1000.0);
        const double ratio = 1e305 / 1.0;
        CHECK(!std::isfinite((std::pow(ratio, 1.0 / span_years) - 1.0) * 100.0));
        CHECK(!std::isfinite((ratio - 1.0) * (1.0 / span_years) * 100.0));
        (void)equity({1, 1e305}, 1, kDay);
    });
    nonfinite("cagr_ratio", "metrics.equity.cagr", [] {
        (void)equity({1e-308, 1e308}, 1e-308, 100 * kYear);
    });
    nonfinite("sub_day_cagr_ratio", "metrics.equity.cagr", [] {
        (void)equity({1e-308, 1e308}, 1e-308, 7 * 60'000);
    });
    nonfinite("sub_day_cagr_simple_overflow", "metrics.equity.cagr", [] {
        (void)equity({1, 1e305}, 1, 7 * 60'000);
    });
    nonfinite("cagr_pct_product", "metrics.equity.cagr", [] {
        (void)equity({1, 1e307});
    });
    nonfinite("drawdown_subtraction", "metrics.equity.max_equity_drawdown", [] {
        (void)equity({kMax, -kMax});
    });
    nonfinite("drawdown_percentage", "metrics.equity.max_equity_drawdown_pct", [] {
        (void)equity({1e-308, -1});
    });
    nonfinite("runup_percentage", "metrics.equity.max_equity_runup_pct", [] {
        (void)equity({1, 1e-308, 0.5});
    });
    nonfinite("recovery_ratio", "metrics.equity.recovery_factor", [] {
        (void)equity({1, 0.75}, 1, kYear, 1e308);
    });
    nonfinite("calmar_ratio", "metrics.equity.calmar", [] {
        (void)equity({1, 1 - 1e-15, 1e306}, 1, kYear / 2);
    });
    nonfinite("monthly_variance", "metrics.equity.sharpe_monthly", [] {
        (void)equity({1, 1e200, 1}, 1, kYear);
    });
    nonfinite("monthly_return_ratio", "metrics.equity.sharpe_monthly", [] {
        (void)equity({1e-308, 1e308, 1e-308}, 1e-308, kYear);
    });
    nonfinite("monthly_return_sum", "metrics.equity.max_equity_runup_pct", [] {
        // The first rebound (1e308 - 1) / 1 * 100 overflows before resampling.
        (void)equity({1, 1e308, 1, 1e308, 1}, 1, kYear);
    });
    nonfinite("monthly_return_sum_isolated", "metrics.equity.sharpe_monthly", [] {
        // Each runup percent is 1e308 (finite), but 200 returns of 1e306
        // overflow their shared sum before either deviation is evaluated.
        std::vector<double> values{1};
        for (int i = 0; i < 200; ++i) {
            values.push_back(1e306);
            values.push_back(1);
        }
        (void)equity(values, 1, kYear);
    });
    nonfinite("monthly_downside_square", "metrics.equity.sortino_monthly", [] {
        // The two admitted returns are identical: variance is zero, but the
        // population downside square overflows. Negative-base steps are
        // skipped by the existing return definition.
        (void)equity({1, -1e200, 1, -1e200}, 1, kYear);
    });
    nonfinite("bar_variance", "metrics.equity.sharpe_bar", [] {
        (void)equity({1, 1e200, 1}, 1, kDay);
    });
    nonfinite("sub_day_bar_variance", "metrics.equity.sharpe_bar", [] {
        (void)equity({1, 1e200, 1}, 1, 60'000);
    });
    nonfinite("bar_return_ratio", "metrics.equity.sharpe_bar", [] {
        (void)equity({1e-308, 1e308, 1e-308}, 1e-308, kDay);
    });
    nonfinite("bar_return_sum", "metrics.equity.max_equity_runup_pct", [] {
        (void)equity({1, 1e308, 1, 1e308, 1}, 1, kDay);
    });
    nonfinite("bar_return_sum_isolated", "metrics.equity.sharpe_bar", [] {
        std::vector<double> values{1};
        for (int i = 0; i < 200; ++i) {
            values.push_back(1e306);
            values.push_back(1);
        }
        // Exactly one day, all 401 points in January: no monthly returns.
        (void)equity(values, 1, kDay / 400);
    });
    nonfinite("sub_day_bar_return_sum_isolated", "metrics.equity.sharpe_bar", [] {
        std::vector<double> values{1};
        for (int i = 0; i < 200; ++i) {
            values.push_back(1e306);
            values.push_back(1);
        }
        (void)equity(values, 1, 60'000);
    });
    nonfinite("bar_downside_square", "metrics.equity.sortino_bar", [] {
        (void)equity({1, -1e200, 1, -1e200}, 1, kDay);
    });
    nonfinite("sub_day_bar_downside_square", "metrics.equity.sortino_bar", [] {
        (void)equity({1, -1e200, 1, -1e200}, 1, 60'000);
    });
}

void annualization_span_rule() {
    row = "seven_minutes_eight_percent";
    const pf_equity_point_t curve[] = {
        {kTime, 10000, 0}, {kTime + 60'000, 9900, 0},
        {kTime + 7 * 60'000, 10800, 0},
    };
    // The frozen walk supplies all thirteen unchanged fields. The two
    // proportional expectations come from the input curve independently.
    const auto want = equity_stats_reference::compute_equity_stats(
        curve, 3, 10000, "UTC", 100, 108, 1, 800);
    CHECK(std::isfinite(want.sharpe_bar) && std::isfinite(want.sortino_bar));
    const double span_years = static_cast<double>(curve[2].time_ms - curve[0].time_ms)
                           / (365.25 * 86400.0 * 1000.0);
    const double ratio = curve[2].equity / 10000.0;
    const double compounded = (std::pow(ratio, 1.0 / span_years) - 1.0) * 100.0;
    CHECK(!std::isfinite(compounded));
    const double simple = (ratio - 1.0) * (1.0 / span_years) * 100.0;
    const double drawdown_pct =
        (curve[0].equity - curve[1].equity) / curve[0].equity * 100.0;
    CHECK(std::isfinite(want.cagr) && std::isfinite(want.calmar));
    CHECK(same_bits(want.cagr, simple));
    CHECK(same_bits(want.calmar, simple / drawdown_pct));
    try {
        const auto got = metrics::compute_equity_stats(
            curve, 3, 10000, "UTC", 100, 108, 1, 800);
        CHECK(same_bits(got.cagr, want.cagr) && same_bits(got.calmar, want.calmar));
        CHECK(equity_stats_reference::first_difference(got, want) == nullptr);
        CHECK(std::isfinite(got.sharpe_bar) && std::isfinite(got.sortino_bar));
        CHECK(same_bits(got.sharpe_bar, want.sharpe_bar));
        CHECK(same_bits(got.sortino_bar, want.sortino_bar));
    } catch (const std::exception&) {
        CHECK(false);
    }
    // Monthly ratios remain their existing defined absence (one month).
    CHECK(std::isnan(want.sharpe_monthly) && std::isnan(want.sortino_monthly));

    row = "just_under_one_day_large_gain";
    try {
        const double years = static_cast<double>(kDay - 1) / (365.25 * 86400.0 * 1000.0);
        const double large_ratio = 1e300 / 1.0;
        const double expected = (large_ratio - 1.0) * (1.0 / years) * 100.0;
        const auto got = equity({1, 1e300}, 1, kDay - 1);
        CHECK(std::isfinite(got.cagr) && same_bits(got.cagr, expected));
        CHECK(std::isnan(got.calmar));  // No drawdown.
        CHECK(std::isfinite(got.buy_hold_return));
    } catch (const std::exception&) {
        CHECK(false);
    }
    row = "exactly_one_day_ordinary_gain";
    const pf_equity_point_t daily[] = {{kTime, 10000, 0}, {kTime + kDay, 10800, 0}};
    const auto daily_want = equity_stats_reference::compute_equity_stats(
        daily, 2, 10000, "UTC", 1, 1, 0, 800);
    const auto daily_got = metrics::compute_equity_stats(
        daily, 2, 10000, "UTC", 1, 1, 0, 800);
    CHECK(std::isfinite(daily_got.cagr));
    CHECK(equity_stats_reference::first_difference(daily_got, daily_want) == nullptr);

    // A zero return has a defined zero CAGR even on a sub-day span.
    row = "sub_day_flat_curve";
    const auto flat = equity({10000, 10000}, 10000, 7 * 60'000);
    CHECK(flat.cagr == 0.0 && std::isnan(flat.calmar));
}

void sub_day_monotonic_returns() {
    row = "sub_day_monotonic_returns";
    const std::int64_t span = 7 * 60'000;
    const double span_years = static_cast<double>(span) / (365.25 * 86400.0 * 1000.0);
    const double capital = 10000.0;
    const double finals[] = {9200.0, 10400.0, 10800.0};
    double previous = -kInf;
    for (const double final_equity : finals) {
        const pf_equity_point_t curve[] = {
            {kTime, capital, 0}, {kTime + span, final_equity, 0},
        };
        const double ratio = final_equity / capital;
        const double expected = (ratio - 1.0) * (1.0 / span_years) * 100.0;
        const auto got = metrics::compute_equity_stats(
            curve, 2, capital, "UTC", 1, 1, 0, final_equity - capital);
        CHECK(std::isfinite(got.cagr));
        CHECK(same_bits(got.cagr, expected));
        CHECK(got.cagr > previous);
        CHECK(final_equity < capital ? got.cagr < 0.0 : got.cagr > 0.0);
        previous = got.cagr;
    }
}

void compounded_overflow_fallback() {
    struct Case { std::int64_t span; double final_equity; bool finite_power; };
    const Case cases[] = {
        {kDay, 1e300, false}, {kDay + 1, 1e300, false}, {3 * kDay, 1e300, false},
        // pow itself fits, but converting its return to percent overflows.
        {kYear - kYear / 100, 1e305, true},
    };
    for (const auto& test : cases) {
        row = test.finite_power ? "compounded_percent_overflow_fallback"
                                : "one_or_several_days_pow_fallback";
        const pf_equity_point_t curve[] = {
            {kTime, 1, 0}, {kTime + test.span, test.final_equity, 0},
        };
        const auto want = equity_stats_reference::compute_equity_stats(
            curve, 2, 1, "UTC", 1, 1, 0, 0);
        const double years = static_cast<double>(test.span) / (365.25 * 86400.0 * 1000.0);
        const double ratio = test.final_equity / 1.0;
        const double power = std::pow(ratio, 1.0 / years);
        CHECK(std::isfinite(power) == test.finite_power);
        const double compounded = (power - 1.0) * 100.0;
        CHECK(!std::isfinite(compounded));
        const double simple = (ratio - 1.0) * (1.0 / years) * 100.0;
        CHECK(std::isfinite(want.cagr));
        CHECK(same_bits(want.cagr, simple));
        try {
            const auto got = metrics::compute_equity_stats(
                curve, 2, 1, "UTC", 1, 1, 0, 0);
            CHECK(same_bits(got.cagr, want.cagr));
            CHECK(equity_stats_reference::first_difference(got, want) == nullptr);
            CHECK(std::isfinite(got.buy_hold_return));
        } catch (const std::exception&) {
            CHECK(false);
        }
    }

    row = "one_day_pow_fallback_with_drawdown";
    const pf_equity_point_t drawdown[] = {
        {kTime, 1, 0}, {kTime + kDay / 2, 0.99, 0}, {kTime + kDay, 10, 0},
    };
    const auto want = equity_stats_reference::compute_equity_stats(
        drawdown, 3, 1, "UTC", 1, 1, 0, 9);
    CHECK(want.max_equity_drawdown_pct > 0.0);
    CHECK(std::isfinite(want.sharpe_bar) && std::isfinite(want.sortino_bar));
    const double years = static_cast<double>(kDay) / (365.25 * 86400.0 * 1000.0);
    const double ratio = drawdown[2].equity / 1.0;
    const double compounded = (std::pow(ratio, 1.0 / years) - 1.0) * 100.0;
    CHECK(!std::isfinite(compounded));
    const double simple = (ratio - 1.0) * (1.0 / years) * 100.0;
    const double drawdown_pct =
        (drawdown[0].equity - drawdown[1].equity) / drawdown[0].equity * 100.0;
    CHECK(std::isfinite(want.cagr) && std::isfinite(want.calmar));
    CHECK(same_bits(want.cagr, simple));
    CHECK(same_bits(want.calmar, simple / drawdown_pct));
    try {
        const auto got = metrics::compute_equity_stats(
            drawdown, 3, 1, "UTC", 1, 1, 0, 9);
        CHECK(std::isfinite(got.cagr) && std::isfinite(got.calmar));
        CHECK(same_bits(got.cagr, want.cagr) && same_bits(got.calmar, want.calmar));
        CHECK(equity_stats_reference::first_difference(got, want) == nullptr);
    } catch (const std::exception&) {
        CHECK(false);
    }
}

void bar_ratio_span_boundary() {
    // Both ratios always equal the frozen walk. Only sub-day CAGR/calmar
    // use the proportional formula; at and above one day compounding fits.
    const std::int64_t spans[] = {7 * 60'000, kDay - 1, kDay, kDay + 1};
    for (const std::int64_t span : spans) {
        row = span < kDay ? "sub_day_finite_bar_ratios" : "one_day_finite_bar_ratios";
        const pf_equity_point_t curve[] = {
            {kTime, 10000, 0}, {kTime + span / 2, 9900, 0},
            {kTime + span, 10020, 0},
        };
        const auto want = equity_stats_reference::compute_equity_stats(
            curve, 3, 10000, "UTC", 1, 1, 0, 20);
        CHECK(std::isfinite(want.cagr) && std::isfinite(want.calmar));
        CHECK(std::isfinite(want.sharpe_bar) && std::isfinite(want.sortino_bar));
        const double years = static_cast<double>(span) / (365.25 * 86400.0 * 1000.0);
        const double ratio = curve[2].equity / 10000.0;
        const double simple = (ratio - 1.0) * (1.0 / years) * 100.0;
        const double compounded = (std::pow(ratio, 1.0 / years) - 1.0) * 100.0;
        CHECK(!same_bits(compounded, simple));
        const double drawdown_pct =
            (curve[0].equity - curve[1].equity) / curve[0].equity * 100.0;
        if (span < kDay) {
            CHECK(same_bits(want.cagr, simple));
            CHECK(same_bits(want.calmar, simple / drawdown_pct));
        } else {
            CHECK(same_bits(want.cagr, compounded));
            CHECK(same_bits(want.calmar, compounded / drawdown_pct));
        }
        try {
            const auto got = metrics::compute_equity_stats(
                curve, 3, 10000, "UTC", 1, 1, 0, 20);
            CHECK(equity_stats_reference::first_difference(got, want) == nullptr);
            CHECK(std::isfinite(got.cagr) && std::isfinite(got.calmar));
            CHECK(std::isfinite(got.sharpe_bar) && std::isfinite(got.sortino_bar));
            CHECK(same_bits(got.cagr, want.cagr));
            CHECK(same_bits(got.calmar, want.calmar));
            CHECK(same_bits(got.sharpe_bar, want.sharpe_bar));
            CHECK(same_bits(got.sortino_bar, want.sortino_bar));
        } catch (const std::exception&) {
            CHECK(false);
        }
    }
}

class ShortSpanReportHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (!entered_) {
            entered_ = true;
            (void)submit({native_order::Transact{1.0}, "entry", ""});
        }
    }
private:
    bool entered_ = false;
};

void short_span_real_report() {
    row = "seven_minutes_real_report";
    ShortSpanReportHost host;
    NativeRunSpec spec;
    spec.identity = {"metrics-short-span", 1};
    spec.input_tf = "1"; spec.script_tf = "1";
    spec.tickerid = "TEST:METRICS";
    spec.timezone = "UTC"; spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = 1; spec.account_fx = 1; spec.price_tick = 1;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i)
        bars.push_back({100, i == 7 ? 900.0 : 100.0, 100,
                        i == 7 ? 900.0 : 100.0, 1, kTime + i * 60'000});
    pf_equity_point_t expected_curve[8]{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double open_profit = i == 0 ? 0.0 : (bars[i].close - bars[1].open) * 1.0;
        expected_curve[i] = {bars[i].timestamp, spec.initial_capital + open_profit, open_profit};
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    ReportC report{};
    try {
        host.fill_report(&report);
        CHECK(report.equity_curve_len == 8 && report.trades_len == 1);
        if (report.equity_curve && report.equity_curve_len == 8) {
            CHECK(report.equity_curve[0].equity == 10000);
            CHECK(report.equity_curve[7].equity == 10800);
            CHECK(report.equity_curve[7].time_ms - report.equity_curve[0].time_ms == 7 * 60'000);
            for (std::size_t i = 0; i < bars.size(); ++i) {
                CHECK(report.equity_curve[i].time_ms == expected_curve[i].time_ms);
                CHECK(same_bits(report.equity_curve[i].equity, expected_curve[i].equity));
            }
        }
        CHECK(report.net_profit == 800);
        auto reference = equity_stats_reference::compute_equity_stats(
            expected_curve, 8, spec.initial_capital, "UTC", 100, 900, 0, 800);
        CHECK(std::isfinite(reference.sharpe_bar) && std::isfinite(reference.sortino_bar));
        // Expected return and span come from the input tape, not ReportC.
        const double final_equity = spec.initial_capital
                                  + (bars.back().close - bars[1].open) * 1.0;
        const double years = static_cast<double>(bars.back().timestamp - bars.front().timestamp)
                           / (365.25 * 86400.0 * 1000.0);
        const double ratio = final_equity / spec.initial_capital;
        reference.cagr = (ratio - 1.0) * (1.0 / years) * 100.0;
        if (reference.max_equity_drawdown > 0.0 && reference.max_equity_drawdown_pct > 0.0)
            reference.calmar = reference.cagr / reference.max_equity_drawdown_pct;
        CHECK(std::isfinite(report.metrics.equity.cagr));
        CHECK(same_bits(report.metrics.equity.cagr, reference.cagr));
        CHECK(same_bits(report.metrics.equity.calmar, reference.calmar));
        CHECK(same_bits(report.metrics.equity.sharpe_bar, reference.sharpe_bar));
        CHECK(same_bits(report.metrics.equity.sortino_bar, reference.sortino_bar));
        double pf_equity_stats_t::*const members[] = {
            &pf_equity_stats_t::max_equity_drawdown, &pf_equity_stats_t::max_equity_drawdown_pct,
            &pf_equity_stats_t::max_equity_runup, &pf_equity_stats_t::max_equity_runup_pct,
            &pf_equity_stats_t::buy_hold_return, &pf_equity_stats_t::buy_hold_return_pct,
            &pf_equity_stats_t::sharpe_monthly, &pf_equity_stats_t::sortino_monthly,
            &pf_equity_stats_t::sharpe_bar, &pf_equity_stats_t::sortino_bar,
            &pf_equity_stats_t::cagr, &pf_equity_stats_t::calmar,
            &pf_equity_stats_t::recovery_factor, &pf_equity_stats_t::time_in_market_pct,
            &pf_equity_stats_t::open_pl,
        };
        for (const auto member : members)
            CHECK(std::isnan(report.metrics.equity.*member) == std::isnan(reference.*member));
        CHECK(host.last_error().empty());
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        // ReportC mirrors pf_report_t member for member at the C boundary;
        // this size check keeps the cast honest if either side changes.
        static_assert(sizeof(ReportC) == sizeof(pf_report_t),
                      "ReportC and pf_report_t must stay layout twins");
        const auto wire = live::native_report_json(reinterpret_cast<const pf_report_t&>(report));
        struct Metric { const char* name; double pf_equity_stats_t::*member; };
        const Metric annualized[] = {
            {"cagr", &pf_equity_stats_t::cagr}, {"calmar", &pf_equity_stats_t::calmar},
            {"sharpe_bar", &pf_equity_stats_t::sharpe_bar},
            {"sortino_bar", &pf_equity_stats_t::sortino_bar},
        };
        for (const auto& field : annualized) {
            const auto& value = wire.at("metrics").at("equity").at(field.name);
            const double expected = reference.*field.member;
            if (std::isnan(expected)) {
                CHECK(value.kind == live::Json::Kind::String && value.value == "NaN");
            } else {
                CHECK(value.kind == live::Json::Kind::Number);
                CHECK(value.value == live::report_number(expected).value);
            }
        }
    } catch (const std::exception&) {
        CHECK(false);
    }
    BacktestEngine::free_report(&report);
}

void defined_absences_and_finite_values() {
    row = "defined_absences";
    const auto empty = stats({}, 0);
    CHECK(empty.net_profit == 0 && empty.gross_profit == 0 && empty.gross_loss == 0);
    CHECK(std::isnan(empty.net_profit_pct));
    CHECK(std::isnan(empty.profit_factor));
    CHECK(std::isnan(empty.avg_trade) && std::isnan(empty.avg_trade_pct));
    CHECK(std::isnan(empty.avg_win) && std::isnan(empty.avg_loss));
    CHECK(std::isnan(empty.largest_win) && std::isnan(empty.largest_loss));
    CHECK(std::isnan(empty.expectancy) && std::isnan(empty.avg_bars_in_trade));
    const auto no_losses = stats({trade(1)}, 0);
    CHECK(std::isnan(no_losses.profit_factor) && std::isnan(no_losses.avg_loss));
    CHECK(std::isnan(no_losses.ratio_avg_win_avg_loss));
    const auto no_wins = stats({trade(-1)}, -1);
    CHECK(std::isnan(no_wins.avg_win) && std::isnan(no_wins.gross_profit_pct));

    const auto absent = equity({}, 0, kYear, 0, kNaN, kNaN);
    CHECK(std::isnan(absent.buy_hold_return) && std::isnan(absent.cagr));
    CHECK(std::isnan(absent.calmar) && std::isnan(absent.recovery_factor));
    CHECK(std::isnan(absent.sharpe_monthly) && std::isnan(absent.sortino_monthly));
    CHECK(std::isnan(absent.time_in_market_pct) && absent.open_pl == 0);
    const auto no_span = equity({1, 1}, 1, 0);
    CHECK(std::isnan(no_span.cagr) && std::isnan(no_span.sharpe_bar));
    const auto zero_final = equity({1, 0});
    CHECK(std::isnan(zero_final.cagr) && std::isnan(zero_final.calmar));
    const auto flat = equity({1, 1, 1}, 1, kDay);
    CHECK(std::isnan(flat.sharpe_bar) && std::isfinite(flat.sortino_bar));
    CHECK(std::isnan(flat.sharpe_monthly) && std::isnan(flat.sortino_monthly));
    const auto monthly_flat = equity({1, 1, 1}, 1, kYear);
    CHECK(std::isnan(monthly_flat.sharpe_monthly) && std::isfinite(monthly_flat.sortino_monthly));
    const auto positive_returns = equity({1, 2, 4});
    CHECK(std::isnan(positive_returns.sharpe_monthly) && std::isnan(positive_returns.sortino_monthly));
    CHECK(std::isnan(positive_returns.sharpe_bar) && std::isnan(positive_returns.sortino_bar));
    const pf_equity_point_t anchor{kTime, 1, 0};
    const auto empty_window = metrics::compute_equity_stats(
        &anchor, 1, 1, "UTC", kNaN, kNaN, 0, 0, 0);
    CHECK(std::isnan(empty_window.time_in_market_pct) && std::isnan(empty_window.cagr));
    CHECK(empty_window.open_pl == 0 && empty_window.max_equity_drawdown == 0);
    const pf_equity_point_t unobserved{kTime, kNaN, kNaN};
    const auto no_marks = metrics::compute_equity_stats(
        &unobserved, 0, 1, "UTC", kNaN, kNaN, 0, 0);
    CHECK(no_marks.open_pl == 0 && std::isnan(no_marks.cagr));

    row = "finite_operation_order";
    const auto finite = equity({100, 90, 110}, 100, kYear / 2, 10, 1, 1.25);
    CHECK(same_bits(finite.buy_hold_return_pct, (1.25 / 1.0 - 1.0) * 100.0));
    CHECK(same_bits(finite.buy_hold_return, 100.0 * (1.25 / 1.0 - 1.0)));
    CHECK(same_bits(finite.cagr, (std::pow(110.0 / 100.0, 1.0) - 1.0) * 100.0));
    CHECK(finite.recovery_factor == 1);
    CHECK(same_bits(finite.calmar, finite.cagr / 10.0));
    const auto large = equity({}, 1e300, kYear, 0, 1, 1e7);
    CHECK(std::isfinite(large.buy_hold_return));
    CHECK(same_bits(large.buy_hold_return, 1e300 * (1e7 / 1.0 - 1.0)));

    row = "inclusive_bar_difference";
    auto wide = trade(0);
    wide.entry_bar_index = std::numeric_limits<std::int32_t>::min();
    wide.exit_bar_index = std::numeric_limits<std::int32_t>::max();
    CHECK(stats({wide}).avg_bars_in_trade == 4294967296.0);

    row = "calendar_difference_before_double_conversion";
    // The pure helper's int64 interval exceeds INT64_MAX even though both
    // endpoints fit, and these years still fit libc's month-key integer.
    // This is arithmetic coverage; the public wire separately bounds times.
    const pf_equity_point_t long_span[] = {
        {-5'000'000'000'000'000'000LL, 1, 0},
        {0, 1, 0},
        { 5'000'000'000'000'000'000LL, 1, 0},
    };
    const auto span = metrics::compute_equity_stats(long_span, 3, 1, "UTC", 1, 1, 0, 0);
    CHECK(span.cagr == 0);
}

void producer_boundaries() {
    struct Field { const char* name; double TradeC::*member; };
    const Field fields[] = {
        {"entry_price", &TradeC::entry_price}, {"exit_price", &TradeC::exit_price},
        {"qty", &TradeC::qty}, {"pnl", &TradeC::pnl}, {"pnl_pct", &TradeC::pnl_pct},
        {"max_runup", &TradeC::max_runup}, {"max_drawdown", &TradeC::max_drawdown},
        {"commission", &TradeC::commission},
    };
    for (double bad : {kNaN, kInf, -kInf}) {
        for (const auto& field : fields) {
            auto t = trade(1);
            t.*field.member = bad;
            const auto path = std::string("trades.") + field.name;
            nonfinite("native_trade_input", path, [&] { (void)stats({t}); });
            pf_trade_t wire{};
            static_assert(sizeof wire == sizeof t, "report trade ABI size differs");
            std::memcpy(&wire, &t, sizeof wire);
            nonfinite("runner_trade", path, [&] { (void)live::report_trade(wire); });
        }
        nonfinite("native_equity_input", "equity_curve.equity", [&] {
            (void)equity({1, bad, 1}, 1, kDay);
        });
        nonfinite("native_open_profit", "equity_curve.open_profit", [&] {
            pf_equity_point_t point{kTime, 1, bad};
            (void)metrics::compute_equity_stats(&point, 1, 1, "UTC", 1, 1, 0, 0);
        });
        nonfinite("runner_summary", "summary.net_pnl", [&] {
            pf_report_t report{};
            report.net_profit = bad;
            (void)live::native_report_json(report);
        });
        nonfinite("runner_curve", "equity_curve.equity", [&] {
            pf_equity_point_t point{kTime, bad, 0};
            pf_report_t report{};
            report.equity_curve = &point;
            report.equity_curve_len = 1;
            (void)live::native_report_json(report);
        });
    }
    struct Metric { const char* name; double pf_equity_stats_t::*member; };
    const Metric metrics[] = {
        {"max_equity_drawdown", &pf_equity_stats_t::max_equity_drawdown},
        {"max_equity_drawdown_pct", &pf_equity_stats_t::max_equity_drawdown_pct},
        {"max_equity_runup", &pf_equity_stats_t::max_equity_runup},
        {"max_equity_runup_pct", &pf_equity_stats_t::max_equity_runup_pct},
        {"buy_hold_return", &pf_equity_stats_t::buy_hold_return},
        {"buy_hold_return_pct", &pf_equity_stats_t::buy_hold_return_pct},
        {"sharpe_tv", &pf_equity_stats_t::sharpe_monthly},
        {"sortino_tv", &pf_equity_stats_t::sortino_monthly},
        {"sharpe_bar", &pf_equity_stats_t::sharpe_bar},
        {"sortino_bar", &pf_equity_stats_t::sortino_bar},
        {"cagr", &pf_equity_stats_t::cagr}, {"calmar", &pf_equity_stats_t::calmar},
        {"recovery_factor", &pf_equity_stats_t::recovery_factor},
        {"time_in_market_pct", &pf_equity_stats_t::time_in_market_pct},
        {"open_pl", &pf_equity_stats_t::open_pl},
    };
    for (const auto& metric : metrics) {
        for (double bad : {kInf, -kInf}) {
            pf_equity_stats_t value{};
            value.*metric.member = bad;
            nonfinite("runner_metric", std::string("metrics.equity.") + metric.name,
                      [&] { (void)live::report_equity_stats(value); });
        }
    }
    struct TradeMetric { const char* name; double pf_trade_stats_t::*member; };
    const TradeMetric trade_metrics[] = {
        {"percent_profitable", &pf_trade_stats_t::percent_profitable},
        {"net_profit", &pf_trade_stats_t::net_profit},
        {"net_profit_pct", &pf_trade_stats_t::net_profit_pct},
        {"gross_profit", &pf_trade_stats_t::gross_profit},
        {"gross_profit_pct", &pf_trade_stats_t::gross_profit_pct},
        {"gross_loss", &pf_trade_stats_t::gross_loss},
        {"gross_loss_pct", &pf_trade_stats_t::gross_loss_pct},
        {"profit_factor", &pf_trade_stats_t::profit_factor},
        {"avg_trade", &pf_trade_stats_t::avg_trade},
        {"avg_trade_pct", &pf_trade_stats_t::avg_trade_pct},
        {"avg_win", &pf_trade_stats_t::avg_win}, {"avg_win_pct", &pf_trade_stats_t::avg_win_pct},
        {"avg_loss", &pf_trade_stats_t::avg_loss}, {"avg_loss_pct", &pf_trade_stats_t::avg_loss_pct},
        {"ratio_avg_win_avg_loss", &pf_trade_stats_t::ratio_avg_win_avg_loss},
        {"largest_win", &pf_trade_stats_t::largest_win},
        {"largest_win_pct", &pf_trade_stats_t::largest_win_pct},
        {"largest_loss", &pf_trade_stats_t::largest_loss},
        {"largest_loss_pct", &pf_trade_stats_t::largest_loss_pct},
        {"commission_paid", &pf_trade_stats_t::commission_paid},
        {"expectancy", &pf_trade_stats_t::expectancy},
        {"avg_bars_in_trade", &pf_trade_stats_t::avg_bars_in_trade},
        {"avg_bars_in_wins", &pf_trade_stats_t::avg_bars_in_wins},
        {"avg_bars_in_losses", &pf_trade_stats_t::avg_bars_in_losses},
    };
    struct Block { const char* name; pf_trade_stats_t pf_metrics_t::*member; };
    const Block blocks[] = {
        {"metrics.all.", &pf_metrics_t::all},
        {"metrics.longs.", &pf_metrics_t::longs},
        {"metrics.shorts.", &pf_metrics_t::shorts},
    };
    for (const auto& block : blocks) {
        for (const auto& metric : trade_metrics) {
            for (double bad : {kInf, -kInf}) {
                pf_report_t report{};
                (report.metrics.*block.member).*metric.member = bad;
                nonfinite("runner_trade_metric", std::string(block.name) + metric.name,
                          [&] { (void)live::native_report_json(report); });
            }
        }
    }
    row = "runner_defined_absence";
    pf_equity_stats_t absent{};
    absent.cagr = kNaN;  // The producer's explicit out-of-domain sentinel.
    const auto value = live::report_equity_stats(absent).at("cagr");
    CHECK(value.kind == live::Json::Kind::String && value.value == "NaN");
    // Recorded script trace values have their separate nonfinite wire convention.
    CHECK(live::report_number(kNaN).value == "NaN");
}
}  // namespace

int main() {
    report_trade_assembly();
    earlier_failure_keeps_its_code();
    public_report_assembly_faults();
    trade_arithmetic();
    equity_arithmetic();
    annualization_span_rule();
    sub_day_monotonic_returns();
    compounded_overflow_fallback();
    bar_ratio_span_boundary();
    short_span_real_report();
    defined_absences_and_finite_values();
    producer_boundaries();
    std::printf("metrics overflow: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
