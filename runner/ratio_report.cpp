#include "ratio_report.hpp"
#include "report.hpp"
#include <pineforge/metrics.hpp>

#include <limits>
#include <vector>

namespace pineforge::live {

void refresh_report_ratios(Json& document, const std::string& chart_timezone) {
    const auto& points = document.at("report").at("equity_curve");
    if (points.kind != Json::Kind::Array)
        throw std::runtime_error("invalid interval report equity curve");
    std::vector<pf_equity_point_t> curve;
    curve.reserve(points.items.size());
    for (const auto& point : points.items)
        curve.push_back({point.at("time_ms").integer<std::int64_t>(),
                         point.at("equity").real(), point.at("open_profit").real()});
    // These four ratios depend only on the curve and civil-month clock.
    // Choose neutral inputs for unrelated buy/hold, exposure and CAGR work;
    // a positive final-equity capital keeps that unused CAGR ratio at 1.
    const auto capital = !curve.empty() && curve.back().equity > 0.0 ? curve.back().equity : 1.0;
    const auto missing_price = std::numeric_limits<double>::quiet_NaN();
    const auto result = pineforge::metrics::compute_equity_stats(
        curve.empty() ? nullptr : curve.data(), static_cast<std::int64_t>(curve.size()),
        capital, chart_timezone, missing_price, missing_price, 0, 0.0);
    auto& equity = document.members["report"].members["metrics"].members["equity"];
    equity.members["sharpe_monthly"] = report_number(result.sharpe_monthly);
    equity.members["sortino_monthly"] = report_number(result.sortino_monthly);
    equity.members["sharpe_bar"] = report_number(result.sharpe_bar);
    equity.members["sortino_bar"] = report_number(result.sortino_bar);
}

}
