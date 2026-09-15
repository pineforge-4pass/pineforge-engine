#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <pineforge/native_order_identity.hpp>

namespace pineforge::compat::pine {

// Adapter-owned facts for the one retained-child/fresh-parent ordering
// exception.  The native matcher sees only handles and ordinary request
// ordering; no source identity or policy predicate crosses this boundary.
enum class OrderPriorityKind : std::uint8_t { Entry = 0, Exit = 1, Other = 2 };

struct OrderPriorityContext {
    bool broker_flat = false;
    bool process_orders_on_close = false;
    bool calc_on_order_fills = false;
    bool coof_scheduler_active = false;
    bool bar_magnifier_enabled = false;
    bool stream_warmup_mode = false;
    bool stream_idle = true;
    int bar_index = -1;
};

struct OrderPriorityCandidate {
    native_order::RequestHandle handle{};
    OrderPriorityKind kind = OrderPriorityKind::Other;
    std::string id;
    std::string from_entry;
    int created_bar = -1;
    std::uint64_t source_sequence = 0;
    std::uint64_t predecessor = 0;
    std::uint64_t recreated_after_named_cancelled = 0;
    std::uint64_t named_cancel_surviving_exit = 0;
    bool created_flat = false;
    bool birth_from_fill = false;
    bool prior_close = false;
    bool at_entry_capacity = false;
    bool stop_limit_activated = false;
    bool default_quantity = false;
    double requested_qty = std::numeric_limits<double>::quiet_NaN();
    double qty_percent = std::numeric_limits<double>::quiet_NaN();
    double stop = std::numeric_limits<double>::quiet_NaN();
    double limit = std::numeric_limits<double>::quiet_NaN();
    double trail_points = std::numeric_limits<double>::quiet_NaN();
    double trail_price = std::numeric_limits<double>::quiet_NaN();
    double trail_offset = std::numeric_limits<double>::quiet_NaN();
    double profit_ticks = std::numeric_limits<double>::quiet_NaN();
    double loss_ticks = std::numeric_limits<double>::quiet_NaN();
    std::string oca_name;
    int oca_type = 0;
};

struct OrderPriorityDecision {
    native_order::RequestHandle parent{};
    native_order::RequestHandle child{};
};

class OrderPriority {
public:
    static constexpr uint64_t schema_version = 1;
    void attach() { attached_ = true; }
    bool attached() const { return attached_; }
    bool retained_parent_first() const { return retained_parent_first_; }
    void metadata(const std::string& key, double value) {
        // One declaration owner, including values received while detached.
        // Metadata never selects Pine execution. Reattachment preserves off.
        if (key == "flat_retained_child_fresh_parent_order")
            retained_parent_first_ = std::isfinite(value) && value > 0.0;
    }
    std::optional<OrderPriorityDecision> select(
        const OrderPriorityContext& ctx,
        const std::vector<OrderPriorityCandidate>& candidates) const;
private:
    bool attached_ = false;
    bool retained_parent_first_ = true;
};

} // namespace pineforge::compat::pine
