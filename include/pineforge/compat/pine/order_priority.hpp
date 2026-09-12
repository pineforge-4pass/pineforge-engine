#pragma once
#include "../../order_priority.hpp"
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace pineforge { inline namespace engine_script_run_v13 { struct PendingOrder; } }
namespace pineforge::compat::pine {

struct OrderPriorityContext {
    bool broker_flat;
    bool process_orders_on_close;
    bool calc_on_order_fills;
    bool coof_scheduler_active;
    bool bar_magnifier_enabled;
    bool stream_warmup_mode;
    bool stream_idle;
    int bar_index;
};

// Pine's bounded retained-child/recreated-parent exception. The complete
// source-shape rule lives in this component; this is an ownership transfer,
// not a generic native activation/dependency scheduler.
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
    std::optional<broker::OrderPriorityDecision> select(
        const OrderPriorityContext& ctx,
        const std::vector<PendingOrder>& book) const;
private:
    bool attached_ = false;
    bool retained_parent_first_ = true;
};

} // namespace pineforge::compat::pine
