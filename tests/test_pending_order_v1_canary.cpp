// A frozen c45 C-v1 reader must retain an exact prefix view of the current
// source-owned PendingOrder projection.
#include <pineforge/pineforge.h>
#include <pineforge/bar.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "fixtures/pending_order_prefix/c45-v1.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pineforge;

namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
}

namespace {

int failures = 0;

#define CHECK(condition) do {                                                     \
    if (!(condition)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                               \
    }                                                                             \
} while (0)

#define PF_PREFIX_FIELD(name)                                                     \
static_assert(offsetof(c45_pending_order_t, name) ==                              \
                  offsetof(pf_pending_order_v1_t, name),                          \
              #name " offset changed");                                         \
static_assert(sizeof(((c45_pending_order_t*)0)->name) ==                          \
                  sizeof(((pf_pending_order_v1_t*)0)->name),                      \
              #name " size changed");
#include "fixtures/pending_order_prefix/c45-fields.inc"
#undef PF_PREFIX_FIELD

static_assert(PF_PENDING_ORDER_STRUCT_VERSION == 1,
              "the frozen reader is a v1 reader");
static_assert(sizeof(c45_pending_order_t) <= sizeof(pf_pending_order_v1_t),
              "the frozen reader must fit within the append-only runtime mirror");

Bar flat_bar(double price, int64_t timestamp) {
    return Bar{price, price, price, price, 1.0, timestamp};
}

class Probe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (bar_index_ == 1) strategy_exit("frozen-reader-exit", "L", na<double>(), 95.0);
    }
};

} // namespace

int main() {
    const std::vector<Bar> bars = {flat_bar(100.0, 0), flat_bar(100.0, 60'000)};
    Probe strategy;
    strategy.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(strategy.pending_order_count() == 1);
    if (strategy.pending_order_count() != 1) return 1;

    c45_pending_order_t frozen_reader;
    std::memset(&frozen_reader, 0xA5, sizeof(frozen_reader));
    CHECK(strategy_pending_order_get(&strategy, 0, &frozen_reader,
                                     sizeof(frozen_reader)) == 0);
    CHECK(frozen_reader.struct_version == 1);
    CHECK(frozen_reader.size == sizeof(pf_pending_order_v1_t));

    pf_pending_order_v1_t current{};
    CHECK(strategy_pending_order_get(&strategy, 0, &current, sizeof(current)) == 0);
    CHECK(std::memcmp(&frozen_reader, &current, sizeof(frozen_reader)) == 0);

    pf_pending_order_v1_t direct{};
    fill_pending_order_mirror(strategy.pending_order_at(0), &direct);
    CHECK(std::memcmp(&frozen_reader, &direct, sizeof(frozen_reader)) == 0);
    CHECK(frozen_reader.short_seed_collision_role == direct.short_seed_collision_role);

    c45_pending_order_t too_small;
    std::memset(&too_small, 0x5C, sizeof(too_small));
    CHECK(strategy_pending_order_get(&strategy, 0, &too_small, 7) == -1);
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&too_small);
    bool unchanged = true;
    for (size_t index = 0; index < sizeof(too_small); ++index)
        unchanged = unchanged && bytes[index] == 0x5C;
    CHECK(unchanged);

    return failures == 0 ? 0 : 1;
}
