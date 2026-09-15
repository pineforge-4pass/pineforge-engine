#pragma once
#include <cmath>
#include <string>

namespace pineforge::compat::pine {

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
private:
    bool attached_ = false;
    bool retained_parent_first_ = true;
};

} // namespace pineforge::compat::pine
