#pragma once

#include <cstdint>
#include <optional>

namespace pineforge {

enum class PositionSide;

inline namespace reservation_expansion_v1 {

struct ReservationExpansionCapture {
    std::int64_t position_cycle = 0;
    PositionSide side;
    std::optional<std::uint64_t> first_later_admission;
};

// Adapter-owned reservation evidence.  It has no matching authority: the
// native cohort owner remains the one executable representation of growth.
class ReservationExpansion {
public:
    void capture(std::uint64_t receiver, std::int64_t cycle, PositionSide side,
                 double capacity);
    void close_population(std::uint64_t admitted_incarnation);
    const std::optional<ReservationExpansionCapture>& capture() const noexcept {
        return capture_;
    }
    bool population_open() const noexcept {
        return capture_ && !capture_->first_later_admission;
    }
    bool owns_exposure(std::int64_t cycle, PositionSide side) const noexcept;
    bool live_all(std::int64_t cycle, PositionSide side) const noexcept {
        return population_open() && owns_exposure(cycle, side);
    }
    void grow(double& qty, std::int64_t before_cycle, PositionSide before_side,
              double before_qty, std::int64_t after_cycle, PositionSide after_side,
              double after_qty, double epsilon) const;
private:
    std::optional<ReservationExpansionCapture> capture_;
};

class ReservationGrowthSource {
public:
    void assign_capture(std::uint64_t source, std::uint64_t receiver);
    const std::optional<std::uint64_t>& reservation_owner() const noexcept {
        return reservation_owner_;
    }
private:
    std::optional<std::uint64_t> reservation_owner_;
};

} // inline namespace reservation_expansion_v1
} // namespace pineforge
