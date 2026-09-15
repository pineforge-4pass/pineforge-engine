#pragma once

#include <pineforge/leg_activation.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace pineforge::compat::pine {

enum class LimitContinuationCause : std::int32_t { LaterSameOpen, FirstHighRecross };

struct LimitContinuation {
    LimitContinuationCause cause = LimitContinuationCause::LaterSameOpen;
    std::uint64_t observed_fill_sequence = 0;
};

// Immutable source placement evidence.  It is retained with the adapter
// snapshot, so rebinding an exit never reinterprets a later owner/price.
struct ExitPlacementEvidence {
    std::int64_t position_cycle = 0;
    int entry_bar = -1;
    int direction = 0;
    double cursor_price = 0.0;
    double stop_level = std::numeric_limits<double>::quiet_NaN();
    double limit_level = std::numeric_limits<double>::quiet_NaN();
    std::optional<LimitContinuation> limit_continuation;
};

class ExitActivationPolicy {
public:
    ExitActivationPolicy() = default;
    explicit ExitActivationPolicy(ExitPlacementEvidence evidence);
    const std::optional<ExitPlacementEvidence>& evidence() const noexcept { return evidence_; }
    bool holds_stop() const noexcept;
    bool holds_limit() const noexcept;
    bool continues_at_later_open() const noexcept;
    ExitLegActivationBounds resolve(std::int64_t owner_cycle, int owner_entry_bar) const;
private:
    std::optional<ExitPlacementEvidence> evidence_;
};

// Pure producer facts from the adapter's current native callback.  The
// selector is deliberately source-layer only; it cannot alter native matching.
struct ExitActivationRequest {
    bool requested_trailing = false;
    bool full_quantity = true;
    bool from_fill = false;
    bool has_from_entry = false;
};

struct ExitActivationContext {
    std::int64_t cycle = 0;
    int bar_index = -1;
    int position_open_bar = -1;
    int direction = 0;
    double cursor_price = std::numeric_limits<double>::quiet_NaN();
    bool fill_recalc = false;
    bool scheduler = false;
    bool magnifier = false;
    bool process_on_close = false;
    bool warmup = false;
    bool stream_idle = true;
    bool after_first_open_fill = false;
    int recalc_leg = 0;
    bool historical_segment = false;
    bool at_extreme = false;
    int historical_point = 0;
    std::uint64_t current_fill = 0;
};

ExitActivationPolicy select_exit_activation(const ExitActivationRequest& request,
                                            double stop, double limit,
                                            const ExitActivationContext& context);

} // namespace pineforge::compat::pine

namespace pineforge { using PineExitActivationPolicy = compat::pine::ExitActivationPolicy; }
