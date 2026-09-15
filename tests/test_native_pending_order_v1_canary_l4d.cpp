// Native-route v1-prefix canary: a frozen c45 reader sees the exact C prefix
// through strategy_pending_order_get, without accessing a retired PendingOrder.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include "fixtures/pending_order_prefix/c45-v1.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)

#define PF_PREFIX_FIELD(name) \
static_assert(offsetof(c45_pending_order_t, name) == offsetof(pf_pending_order_v1_t, name), \
              #name " offset changed"); \
static_assert(sizeof(((c45_pending_order_t*)0)->name) == sizeof(((pf_pending_order_v1_t*)0)->name), \
              #name " size changed");
#include "fixtures/pending_order_prefix/c45-fields.inc"
#undef PF_PREFIX_FIELD

Bar flat(double price, std::int64_t timestamp) { return {price, price, price, price, 1, timestamp}; }
class CanaryProbe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_exit("frozen-reader-exit", "L", na<double>(), 95.0);
    }
};
}

int main() {
    static_assert(PF_PENDING_ORDER_STRUCT_VERSION == 1, "frozen reader is v1");
    static_assert(sizeof(c45_pending_order_t) <= sizeof(pf_pending_order_v1_t), "prefix must fit");
    const std::vector<Bar> bars = {flat(100, 0), flat(100, 60'000)};
    CanaryProbe probe;
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(strategy_pending_orders_len(&probe) == 1);
    c45_pending_order_t old{};
    pf_pending_order_v1_t current{};
    CHECK(strategy_pending_order_get(&probe, 0, &old, sizeof(old)) == 0);
    CHECK(strategy_pending_order_get(&probe, 0, &current, sizeof(current)) == 0);
    CHECK(old.struct_version == 1 && old.size == sizeof(current));
    CHECK(std::memcmp(&old, &current, sizeof(old)) == 0);
    std::printf("native pending-v1 canary: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
