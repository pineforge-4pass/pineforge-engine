#pragma once

#include <pineforge/reservation_expansion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pineforge::compat::pine {

// Source-side selection evidence.  The generic core keeps ownership and
// match-time cohort resolution; this merely decides whether an adapter exit
// receives a historical reservation-growth receipt.
struct ReservationGrowthCandidate {
    std::uint64_t incarnation = 0;
    std::string source_id;
    bool market_entry = false;
    bool from_fill = false;
    bool at_entry_capacity = false;
    bool is_long = true;
    PositionSide created_position_side = static_cast<PositionSide>(0);
    int created_bar = -1;
};

std::vector<std::uint64_t> select_reservation_growth_sources(
    const std::vector<ReservationGrowthCandidate>& candidates,
    const std::string& from_entry, bool process_on_close, bool effectively_flat,
    double percent, int bar, PositionSide side);
bool admits_reservation_expansion(const std::vector<std::uint64_t>& selected,
                                  bool partial, double reserved, double live) noexcept;

} // namespace pineforge::compat::pine
