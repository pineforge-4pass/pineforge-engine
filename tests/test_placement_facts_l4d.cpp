// A29 native-route twin: placement facts come from a real pending C row.
#include "l8d_twin_support.hpp"

#include <cstdio>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); } } while (0)
struct PlacementRow { bool prior_close = false; double equity = 0.0; };
bool placement_has_prior_close(const PlacementRow& row) { return row.prior_close; }

class Probe final : public source::L4dPineHost {
public:
    Probe() { configure_pine_strategy(fixed_config()); }
    pf_pending_order_v1_t row{}; bool copied = false;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("E", true, missing, 110.0, 1.0);
        const auto rows = pending_rows(this);
        if (!rows.empty()) { row = rows.front(); copied = true; }
    }
};
} // namespace

int main() {
    Probe probe; const Bar bar = point(100, 60'000); probe.run(&bar, 1);
    PlacementRow order{probe.row.created_after_position_close_in_bar != 0,
                       probe.row.explicit_placement_equity};
    const bool expected_close = false;
    CHECK(placement_has_prior_close(order)==expected_close);
    CHECK(probe.last_error().empty());
    CHECK(probe.copied);
    CHECK(std::strcmp(probe.row.id, "E") == 0);
    CHECK(probe.row.created_bar == 0);
    CHECK(probe.row.created_seq != 0);
    CHECK(probe.row.incarnation != 0);
    return failures == 0 ? 0 : 1;
}
