#pragma once
#include "../../bar.hpp"
#include "../../leg_activation.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace pineforge {
enum class PositionSide;
inline namespace engine_script_run_v12 { struct PendingOrder; }
}
namespace pineforge::compat::pine {

enum class LimitContinuationCause : int32_t { LaterSameOpen, FirstHighRecross };
struct LimitContinuation {
    LimitContinuationCause cause;
    // Sequence observed when policy was selected; callback cause remains the
    // independent immutable OrderBirth interval on the order.
    uint64_t observed_fill_sequence;
};

// Immutable original policy evidence. Rebinding must not recompute original
// marketability from the new owner's side or a later-mutated trigger level.
struct ExitPlacementEvidence {
    int64_t position_cycle;
    int entry_bar;
    int direction;
    double cursor_price;
    double stop_level;
    double limit_level;
    std::optional<LimitContinuation> limit_continuation;
};

class ExitActivationPolicy {
public:
    ExitActivationPolicy() = default;
    explicit ExitActivationPolicy(ExitPlacementEvidence evidence) : evidence_(evidence) {
        if (evidence.position_cycle <= 0 || evidence.entry_bar < 0
            || (evidence.direction != 1 && evidence.direction != -1)
            || !std::isfinite(evidence.cursor_price))
            throw std::invalid_argument("invalid Pine exit placement evidence");
    }
    const std::optional<ExitPlacementEvidence>& evidence() const { return evidence_; }
    bool holds_stop() const;
    bool holds_limit() const;
    bool continues_at_later_open() const;
    ExitLegActivationBounds resolve(int64_t owner_cycle, int owner_entry_bar) const;
private:
    std::optional<ExitPlacementEvidence> evidence_;
};

// Transient producer facts, never retained as a parallel mutable mode bag.
struct ExitActivationContext {
    const Bar& bar;
    PositionSide side;
    int64_t cycle;
    int bar_index;
    int position_open_bar;
    int position_entry_count;
    double position_quantity;
    int pyramiding;
    std::size_t lot_count;
    const std::string& first_lot_id;
    uint64_t first_lot_incarnation;
    bool fill_recalc;
    bool scheduler;
    double cursor_price;
    bool after_first_open_fill;
    int recalc_leg;
    bool historical_segment;
    bool at_extreme;
    int historical_point;
    uint64_t market_recalc_incarnation;
    uint64_t market_recalc_fill;
    uint64_t current_fill;
    bool magnifier;
    bool process_on_close;
    bool warmup;
    bool stream_idle;
    bool pending_empty;
    int slippage;
    double pointvalue;
    double account_fx;
    bool fx_series_empty;
    double tick_high;
};

ExitActivationPolicy select_exit_activation(const PendingOrder& order,
    double requested_stop, double requested_limit, const ExitActivationContext& context);

} // namespace pineforge::compat::pine

namespace pineforge { using PineExitActivationPolicy = compat::pine::ExitActivationPolicy; }
