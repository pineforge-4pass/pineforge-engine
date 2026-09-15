// L3a C projection witness. The v1 POD layout remains frozen while its
// values are read from PendingIntentView / adapter placement facts.
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

int failures = 0;

#define CHECK(value) do {                                                        \
    if (!(value)) {                                                              \
        std::fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #value); \
        ++failures;                                                              \
    }                                                                            \
} while (0)

class ProjectionHost final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const pineforge::Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("C-projection", true,
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN(), 2.0,
                           "pending-view");
        }
    }
};

} // namespace

int main() {
    ProjectionHost host;
    const pineforge::Bar bar{100.0, 101.0, 99.0, 100.0, 1.0, 60000};
    host.run(&bar, 1);
    const auto handle = static_cast<pf_strategy_t>(&host);

    CHECK(strategy_pending_orders_len(handle) == 1);
    pf_pending_order_v1_t row{};
    CHECK(strategy_pending_order_get(handle, 0, &row, sizeof(row)) == 0);
    CHECK(row.struct_version == 1);
    CHECK(row.size == sizeof(row));
    CHECK(std::strcmp(row.id, "C-projection") == 0);
    CHECK(row.type == 1);
    CHECK(row.is_long == 1);
    CHECK(row.qty == 2.0);
    CHECK(row.incarnation != 0);
    CHECK(std::strcmp(row.comment, "pending-view") == 0);
    CHECK(row.short_seed_collision_role == 0);

    double qty = std::numeric_limits<double>::quiet_NaN();
    int close_only = -1;
    int partition = -1;
    CHECK(strategy_pending_order_fill_qty(handle, 0, 100.0, &qty, &close_only,
                                          &partition) == 0);
    CHECK(qty == 2.0 && close_only == 0 && partition == 0);
    CHECK(strategy_pending_order_level_resolved(handle, 0) == 1);
    CHECK(strategy_position_size(handle) == 0.0);
    return failures == 0 ? 0 : 1;
}
