#pragma once

#include <cstdint>

namespace pineforge::metrics::detail {

#define PF_ANNUALIZATION_SPAN_MS(value) inline constexpr std::uint64_t kMinAnnualizationSpanMs = value;
#include "../docker/report_annualization.def"
#undef PF_ANNUALIZATION_SPAN_MS

}  // namespace pineforge::metrics::detail
