#pragma once

#include <cmath>
#include <cstddef>
#include <string>

namespace pineforge::source {

inline double tv_money_round(double value) {
    if (!std::isfinite(value) || value == 0.0) return value;
    const double magnitude = std::floor(std::log10(std::abs(value)));
    const double scale = std::pow(10.0, 9.0 - magnitude);
    const double rounded = std::floor(std::abs(value) * scale + 0.5) / scale;
    return value < 0.0 ? -rounded : rounded;
}

inline double tv_money_floor_lot(double qty, double step) {
    if (!(step > 0.0) || !std::isfinite(qty) || qty <= 0.0) return qty;
    double floored = std::floor(qty / step) * step;
    if (step == 0.01) {
        const double cent_candidate = std::floor(qty * 100.0) * step;
        if (cent_candidate > floored && cent_candidate <= qty)
            floored = cent_candidate;
    }
    return floored < qty ? floored : qty;
}

inline const std::string& pine_enum_str_at(const std::string* table, std::size_t n,
                                           int idx) {
    static const std::string kEmpty;
    if (n == 0 || table == nullptr) return kEmpty;
    std::size_t u = static_cast<std::size_t>(idx);
    if (u >= n) u = n - 1;
    return table[u];
}

} // namespace pineforge::source
