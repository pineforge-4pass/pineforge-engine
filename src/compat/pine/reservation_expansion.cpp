#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/reservation_expansion.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include "../../engine_internal.hpp"

namespace pineforge::compat::pine {
std::vector<uint64_t> select_reservation_growth_sources(const std::vector<source::PendingOrder>& book,
        const std::string& from_entry, bool process_on_close, bool effectively_flat,
        double percent, int bar, PositionSide side) {
    if (!from_entry.empty() || !process_on_close || effectively_flat
        || percent < 100.0 - internal::kFullPercentEps) return {};
    std::vector<uint64_t> selected;
    for (const auto& source : book) {
        if (source.type != OrderType::MARKET && source.type != OrderType::ENTRY
            && source.type != OrderType::RAW_ORDER) continue;
        const auto requested = source.is_long ? PositionSide::LONG : PositionSide::SHORT;
        if (source.created_bar != bar || source.type != OrderType::MARKET
            || source.birth.from_fill() || placement_at_entry_capacity(source)
            || requested != side || source.created_position_side != side) return {};
        selected.push_back(source.incarnation);
    }
    return selected;
}
bool admits_reservation_expansion(const std::vector<uint64_t>& selected,
        bool partial, double reserved, double live) {
    return !selected.empty() && !partial && std::isfinite(reserved)
        && reserved >= live - internal::kFullQtyEps;
}
} // namespace pineforge::compat::pine
