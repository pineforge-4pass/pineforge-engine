#pragma once

#include <pineforge/pineforge.h>
#include <cstdint>

namespace pineforge::metrics::detail {

#define PF_ANNUALIZATION_SPAN_MS(value) inline constexpr std::uint64_t kMinAnnualizationSpanMs = value;
#define PF_SHORT_SPAN_ANNUALIZED_FIELD(field)
#include "../docker/report_annualization.def"
#undef PF_SHORT_SPAN_ANNUALIZED_FIELD
#undef PF_ANNUALIZATION_SPAN_MS

inline constexpr double pf_equity_stats_t::*kShortSpanAnnualizedFields[] = {
#define PF_ANNUALIZATION_SPAN_MS(value)
#define PF_SHORT_SPAN_ANNUALIZED_FIELD(field) &pf_equity_stats_t::field,
#include "../docker/report_annualization.def"
#undef PF_SHORT_SPAN_ANNUALIZED_FIELD
#undef PF_ANNUALIZATION_SPAN_MS
};

inline bool short_span_absence(double span_ms, double pf_equity_stats_t::*field) {
    if (span_ms >= static_cast<double>(kMinAnnualizationSpanMs)) return false;
    for (const auto member : kShortSpanAnnualizedFields)
        if (member == field) return true;
    return false;
}

}  // namespace pineforge::metrics::detail
