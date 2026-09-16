// A29 native-route twin: paired-market admission is read from the real C mirror.
#include "l8d_twin_support.hpp"

#include <cstdio>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); } } while (0)

class Matrix final : public source::L4dPineHost {
public:
    Matrix() { configure_pine_strategy(fixed_config(10'000.0, 1.0, 1)); set_margin_call_enabled(false); }
    pf_pending_order_v1_t sell{}, buy{};
    bool copied = false;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("S", false, missing, missing, 3.0);
        strategy_entry("B", true, missing, missing, 2.0);
        copied = strategy_pending_order_get(this, 0, &sell, sizeof sell) == 0
            && strategy_pending_order_get(this, 1, &buy, sizeof buy) == 0;
        const auto& m = sell;
        CHECK(m.size==sizeof(m));
    }
};
} // namespace

int main() {
    const Bar bars[] = {point(100, 60'000), point(100, 120'000), point(100, 180'000)};
    Matrix matrix; matrix.run(bars, 3, "1", "1");
    CHECK(matrix.copied);
    CHECK(std::strcmp(matrix.sell.id, "S") == 0);
    CHECK(std::strcmp(matrix.buy.id, "B") == 0);
    CHECK(matrix.sell.pine_frozen_market_instruction_own_units == 3.0);
    CHECK(matrix.buy.pine_frozen_market_instruction_transaction_units == 5.0);
    CHECK(matrix.live_position_size() == 2.0);
    CHECK(matrix.trade_count() == 1);
    return failures == 0 ? 0 : 1;
}
