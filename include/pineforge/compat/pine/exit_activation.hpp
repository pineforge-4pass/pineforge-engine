#pragma once

#include <pineforge/bar.hpp>
#include <pineforge/compat/pine/order_birth.hpp>
#include <pineforge/leg_activation.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
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
    HistoricalBirthReach birth_reach = HistoricalBirthReach::Standard;
    std::string_view from_entry{};
    std::string_view oca_name{};
    double quantity = std::numeric_limits<double>::quiet_NaN();
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
    Bar bar{};
    int position_entry_count = 0;
    double position_quantity = 0.0;
    int pyramiding = 0;
    std::size_t lot_count = 0;
    std::string_view first_lot_id{};
    std::uint64_t first_lot_incarnation = 0;
    std::uint64_t market_recalc_incarnation = 0;
    std::uint64_t market_recalc_fill = 0;
    bool pending_empty = false;
    int slippage = 0;
    double pointvalue = 1.0;
    double account_fx = 1.0;
    bool fx_series_empty = true;
    bool bar_path_high_first = false;
    double tick_high = std::numeric_limits<double>::quiet_NaN();
};

ExitActivationPolicy select_exit_activation(const ExitActivationRequest& request,
                                            double stop, double limit,
                                            const ExitActivationContext& context);

} // namespace pineforge::compat::pine

namespace pineforge { using PineExitActivationPolicy = compat::pine::ExitActivationPolicy; }
