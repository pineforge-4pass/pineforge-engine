#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace pineforge {
enum class PositionSide;
inline namespace engine_script_run_v12 { struct PendingOrder; }
}
namespace pineforge::compat::pine {
// Called only for omitted explicit exit qty. Pine owns the population selector;
// native capture/closure/resize never reads these compatibility settings.
std::vector<uint64_t> select_reservation_growth_sources(const std::vector<PendingOrder>& book,
    const std::string& from_entry, bool process_on_close, bool effectively_flat,
    double percent, int bar, PositionSide side);
bool admits_reservation_expansion(const std::vector<uint64_t>& selected,
    bool partial, double reserved, double live);
} // namespace pineforge::compat::pine
