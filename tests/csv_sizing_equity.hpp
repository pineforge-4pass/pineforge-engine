#pragma once

#include <cmath>
#include <cstdio>
#include <limits>

// Test-side uncertainty in equity reconstructed from Net PnL cells rounded
// to 12 significant decimal digits. This does not bound upstream rounding in
// the capture or substitute for the decoded report's equity.
namespace csv_sizing_equity {

struct PnlError {
    long double csv = 0.0L;
    long double absolute_pnl = 0.0L;
    unsigned cells = 0;

    void add(double pnl) {
        ++cells;
        const double magnitude = std::abs(pnl);
        absolute_pnl += magnitude;
        if (magnitude == 0.0) return; // significant-digit rounding preserves zero
        // Include binary64 parsing at powers of ten when selecting the
        // decimal place; take half a unit of the 12th significant digit.
        const long double upper = std::nextafter(magnitude, std::numeric_limits<double>::infinity());
        csv += 0.5L * std::pow(10.0L, std::floor(std::log10(upper)) - 11.0L);
    }
};

inline bool lot_floor_decidable(const char* tape, int trade, const char* basis,
                               double equity, const PnlError& error, double fraction,
                               double price, long double lot, long double floor_bias) {
    if (!std::isfinite(equity) || equity <= 0 || !std::isfinite(price) || price <= 0
        || !std::isfinite(error.csv) || !std::isfinite(error.absolute_pnl)) {
        std::printf("NOT COMPARED %s trade=%d %s: expected positive finite equity and price, "
                    "finite PnL sum bound; got equity=%.17g price=%.17g bound=%.17Lg\n",
                    tape, trade, basis, equity, price, error.csv);
        return false;
    }
    const long double scale = static_cast<long double>(fraction) / price;
    const long double units = scale * equity;
    const long double lots = units / lot;
    const long double distance = std::abs(lots - std::round(lots)) * lot;
    const long double csv_bound = error.csv * scale;
    // Also allow for binary64 parsing, accumulation and conversion to lots
    // in the original check, and its existing positive bias before flooring.
    const long double epsilon = std::numeric_limits<double>::epsilon();
    const long double numeric_bound = epsilon *
        ((error.cells + 2.0L) * (std::abs(equity) + error.absolute_pnl) * scale
         + 8.0L * std::abs(units));
    const long double bound = csv_bound + numeric_bound + floor_bias * lot;
    const bool decided = distance > bound;
    std::printf("%s %s trade=%d %s: equity=%.17g price=%.17g lot=%.17Lg cells=%u "
                "distance_to_lot=%.17Lg csv_B=%.17Lg total_bound=%.17Lg\n",
                decided ? "CSV EQUITY" : "NOT COMPARED", tape, trade, basis, equity, price,
                lot, error.cells, distance, csv_bound, bound);
    return decided;
}

}  // namespace csv_sizing_equity
