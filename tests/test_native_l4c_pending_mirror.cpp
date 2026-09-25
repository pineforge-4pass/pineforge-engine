#include <pineforge/source/pine_native_host.hpp>
#include <pineforge/pending_order_mirror.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (0)

class MirrorProbe final : public source::PineNativeHost {
public:
    MirrorProbe() {
        source::PineStrategyConfig config;
        config.process_orders_on_close = true;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 10;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("L", true, 50.0);
            strategy_exit("X", "L", 105.0, std::numeric_limits<double>::quiet_NaN(),
                          10.0, std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::quiet_NaN(), 100.0);
        }
    }
};

class DualStopProbe final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("L", true, std::numeric_limits<double>::quiet_NaN(), 105.0);
            strategy_entry("S", false, std::numeric_limits<double>::quiet_NaN(), 95.0);
        }
    }
};

void projection_comes_from_live_facts() {
    MirrorProbe probe;
    const Bar bars[] = {{100,100,100,100,1,1000}, {100,100,100,100,1,2000},
                        {100,100,100,100,1,3000}};
    probe.run(bars, 3);
    CHECK(probe.last_error().empty());
    const auto& view = probe.pending_intent_view();
    CHECK(view.size() >= 1);
    if (view.size() > 0) {
        pf_pending_order_v1_t row{};
        int target = 0;
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t candidate{};
            if (view.copy_v1(i, &candidate) == 0 && std::strcmp(candidate.from_entry, "L") == 0) {
                target = i;
                row = candidate;
                break;
            }
        }
        CHECK(view.copy_v1(target, &row) == 0);
        CHECK(row.struct_version == 1 && row.size == sizeof(row));
        CHECK(row.birth_cause != static_cast<std::int32_t>(OrderBirthCause::Unattributed));
        CHECK(row.birth_bar >= 0);
        CHECK(row.coof_cascade_seg_i == -1 || row.coof_cascade_seg_i >= 0);
        CHECK(row.cancellation_state == 0 || row.cancellation_state == 1);
        double stop = 0.0, limit = 0.0, trail_activation = 0.0;
        CHECK(view.effective_levels(target, &stop, &limit, &trail_activation) >= 0);
        CHECK(std::isfinite(trail_activation) || std::isnan(trail_activation));
    }
}

void dual_path_is_a_live_adapter_projection() {
    DualStopProbe probe;
    const Bar bars[] = {{100,101,99,100,1,1000}, {100,110,90,100,1,2000}};
    probe.run(bars, 2);
    CHECK(probe.last_error().empty());
    // Expectation corrected (R5 INT24, ruling on B-ADAPTER finding 1): the
    // second bar is a tie (|H - O| = |O - L| = 10), whose low leg the kernel
    // walks first, filling the short entry first; the observation reported 1
    // under its own `<=` tie rule and now names the leg the run walks.
    CHECK(probe.last_bar_dual_entry_path() == 2);
}
} // namespace

int main() {
    projection_comes_from_live_facts();
    dual_path_is_a_live_adapter_projection();
    std::printf("L4c live pending mirror: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
