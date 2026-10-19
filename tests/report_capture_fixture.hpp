#pragma once

#include <pineforge/engine.hpp>

#include "equity_stats_reference.hpp"
#include "report_failure_checks.hpp"

#include <limits>
#include <stdexcept>
#include <vector>

// Test-owned observation of present_report, before metric assembly. This is
// execution evidence, not a successfully published canonical result. It lets
// a report-failure row retain its trade/curve assertions without fabricating
// missing metrics or changing the tape. Never pass raw to free_report.
struct ReportCaptureFixture {
    pineforge::ReportC raw{};
    std::vector<pineforge::TradeC> trades;
    std::vector<pf_equity_point_t> curve;

    ReportCaptureFixture() = default;
    ReportCaptureFixture(const ReportCaptureFixture&) = delete;
    ReportCaptureFixture& operator=(const ReportCaptureFixture&) = delete;

    void save(const pineforge::ReportC& value) {
        trades.clear();
        curve.clear();
        if (value.trades && value.trades_len > 0)
            trades.assign(value.trades, value.trades + value.trades_len);
        if (value.equity_curve && value.equity_curve_len > 0)
            curve.assign(value.equity_curve, value.equity_curve + value.equity_curve_len);
        raw = value;
        raw.trades = trades.empty() ? nullptr : trades.data();
        raw.equity_curve = curve.empty() ? nullptr : curve.data();
        raw.metrics = {};
        raw.security_diag = nullptr; raw.security_diag_len = 0;
        raw.trace = nullptr; raw.trace_len = 0;
        raw.trace_names = nullptr; raw.trace_names_len = 0;
        raw.broker_state_hash = nullptr; raw.broker_state_hash_len = 0;
    }
};

// Read through the real public entry, with the original presentation as an
// independent input to the frozen reference walk. Only the specifically
// predicted annualization overflow (or its earlier latched execution failure)
// is an expected rejection. Callers inspect `capture` as execution evidence
// on false, and the actual published report on true.
inline bool read_report_with_annualization_check(
    const pineforge::BacktestEngine& host, pineforge::ReportC* out,
    const ReportCaptureFixture& capture, double capital) {
    const auto expected_field = [&]() {
        const double absent = std::numeric_limits<double>::quiet_NaN();
        const auto reference = equity_stats_reference::compute_equity_stats(
            capture.raw.equity_curve, capture.raw.equity_curve_len, capital,
            "UTC", absent, absent, 0, capture.raw.net_profit);
        return report_failure_checks::annualization_overflow(reference);
    };
    try {
        host.fill_report(out);
    } catch (const std::exception& error) {
        const char* field = expected_field();
        if (!field) throw;
        if (!host.last_error().empty()) {
            const auto earlier = pineforge::run_failure_value_of(host);
            const auto carried = pineforge::classify_run_failure(error);
            if (dynamic_cast<const pineforge::RunFailureInfo*>(&error) == nullptr
                || earlier.code == pineforge::RunFailureCode::none
                || carried.code != earlier.code || carried.args != earlier.args
                || host.last_error() != error.what()) throw;
        } else if (!report_failure_checks::is_nonfinite(error, field)) {
            throw;
        }
        return false;
    }
    // A pre-admission refusal is allowed to return empty even when an older
    // presentation remains in this external observer.
    if (out->equity_curve_len > 0 && expected_field())
        throw std::logic_error("report fixture: an annualization overflow was published");
    return true;
}
