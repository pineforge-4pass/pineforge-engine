#include "ratio_report.hpp"
#include "report.hpp"
#include <pineforge/metrics.hpp>
#include <pineforge/metrics_stream.hpp>
#include <pineforge/spill_curve_reader.hpp>

#include <limits>
#include <vector>

namespace pineforge::live {

namespace {

pf_equity_point_t curve_point(const Json& point) {
    return {point.at("time_ms").integer<std::int64_t>(),
            point.at("equity").real(), point.at("open_profit").real()};
}

void put_ratios(Json& document, const pf_equity_stats_t& result) {
    auto& equity = document.members["report"].members["metrics"].members["equity"];
    equity.members["sharpe_monthly"] = report_number(result.sharpe_monthly);
    equity.members["sortino_monthly"] = report_number(result.sortino_monthly);
    equity.members["sharpe_bar"] = report_number(result.sharpe_bar);
    equity.members["sortino_bar"] = report_number(result.sortino_bar);
}

// The positional reader of metrics::SpillCurve over the serialized curve items.
void read_spooled_points(const void* context, std::int64_t index, std::int64_t count,
                         pf_equity_point_t* output) {
    const auto& curve = *static_cast<const SpooledReportArray*>(context);
    curve.visit(static_cast<std::size_t>(index), static_cast<std::size_t>(count),
                [&](std::string_view item) { *output++ = curve_point(parse_json(item)); });
}

} // namespace

void refresh_report_ratios(Json& document, const std::string& chart_timezone) {
    const auto& points = document.at("report").at("equity_curve");
    if (points.kind != Json::Kind::Array)
        throw std::runtime_error("invalid interval report equity curve");
    std::vector<pf_equity_point_t> curve;
    curve.reserve(points.items.size());
    for (const auto& point : points.items)
        curve.push_back(curve_point(point));
    // These four ratios depend only on the curve and civil-month clock.
    // Choose neutral inputs for unrelated buy/hold, exposure and CAGR work;
    // a positive final-equity capital keeps that unused CAGR ratio at 1.
    const auto capital = !curve.empty() && curve.back().equity > 0.0 ? curve.back().equity : 1.0;
    const auto missing_price = std::numeric_limits<double>::quiet_NaN();
    const pf_equity_point_t* curve_points = curve.empty() ? nullptr : curve.data();
    const auto result = pineforge::metrics::compute_equity_stats(
        curve_points, static_cast<std::int64_t>(curve.size()),
        capital, chart_timezone, missing_price, missing_price, 0, 0.0);
    put_ratios(document, result);
}

void refresh_report_ratios(SpooledReportDocument& document, const std::string& chart_timezone) {
    const auto* spooled = document.array("equity_curve");
    if (!spooled) {
        // A whole-document row can leave the curve in the scalar tree.
        refresh_report_ratios(document.scalars(), chart_timezone);
        return;
    }
    const auto count = static_cast<std::int64_t>(spooled->size());
    // Convert every point once, in order, before any metric walk: a malformed
    // item is reported first, as it is when the curve is staged as an array.
    pf_equity_point_t last{};
    spooled->visit(0, spooled->size(),
                   [&](std::string_view item) { last = curve_point(parse_json(item)); });
    const auto capital = count && last.equity > 0.0 ? last.equity : 1.0;
    const auto missing_price = std::numeric_limits<double>::quiet_NaN();
    const pineforge::metrics::ScratchCurveReader reader{
        spooled, count, count ? &read_spooled_points : nullptr};
    const pineforge::metrics::SpillCurve curve(reader);
    const auto result = pineforge::metrics::compute_equity_stats<pineforge::metrics::SpillCurve>(
        curve, count, capital, chart_timezone, missing_price, missing_price,
        std::int64_t{0}, 0.0);
    put_ratios(document.scalars(), result);
}

}
