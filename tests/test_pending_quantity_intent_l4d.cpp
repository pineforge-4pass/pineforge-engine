// A29 native-route twin: public quantity-intent fields preserve deferred percent.
#include "l8d_twin_support.hpp"

#include <cstdio>
#include <optional>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); } } while (0)
struct RequestView {
    std::optional<double> intent_value;
    std::optional<double> reservation_value;
    const std::optional<double>& intent() const { return intent_value; }
    const std::optional<double>& reservation() const { return reservation_value; }
};

class Probe final : public source::L4dPineHost {
public:
    Probe() { configure_pine_strategy(fixed_config()); }
    pf_pending_order_v1_t row{}; bool copied = false;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_exit("X", "E", missing, 95.0, missing, missing, missing, 50.0);
        const auto rows = pending_rows(this);
        if (!rows.empty()) { row = rows.front(); copied = true; }
    }
};
} // namespace

int main() {
    RequestView request;
    CHECK(!request.intent() && !request.reservation());
    Probe probe; const Bar bar = point(100, 60'000); probe.run(&bar, 1);
    CHECK(probe.last_error().empty());
    CHECK(probe.copied);
    CHECK(probe.row.qty_percent == 50.0);
    CHECK(probe.row.quantity_intent_kind == 3U);
    CHECK(probe.row.quantity_intent_numerator == 50.0);
    CHECK(probe.row.quantity_intent_denominator == 100.0);
    CHECK(probe.row.quantity_reservation_present == 0U);
    return failures == 0 ? 0 : 1;
}
