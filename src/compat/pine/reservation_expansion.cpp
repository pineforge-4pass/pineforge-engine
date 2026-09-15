#include <pineforge/compat/pine/reservation_expansion.hpp>

#include <pineforge/engine.hpp>

#include <cmath>

namespace pineforge::compat::pine {

std::vector<std::uint64_t> select_reservation_growth_sources(
        const std::vector<ReservationGrowthCandidate>& candidates,
        const std::string& from_entry, bool process_on_close, bool effectively_flat,
        double percent, int bar, PositionSide side) {
    if (!from_entry.empty() || !process_on_close || effectively_flat
        || percent < 100.0 - 1e-9) {
        return {};
    }
    std::vector<std::uint64_t> selected;
    for (const auto& candidate : candidates) {
        const auto requested = candidate.is_long ? PositionSide::LONG : PositionSide::SHORT;
        if (!candidate.market_entry || candidate.from_fill || candidate.at_entry_capacity
            || candidate.created_bar != bar || requested != side
            || candidate.created_position_side != side) {
            return {};
        }
        selected.push_back(candidate.incarnation);
    }
    return selected;
}

bool admits_reservation_expansion(const std::vector<std::uint64_t>& selected,
                                  bool partial, double reserved, double live) noexcept {
    return !selected.empty() && !partial && std::isfinite(reserved)
        && reserved >= live - 1e-9;
}

} // namespace pineforge::compat::pine
