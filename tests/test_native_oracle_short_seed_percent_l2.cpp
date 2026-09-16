// Native-route percent-sizing companion to the L0 ShortSeed oracle.  It keeps
// the finding-272 command book but uses the percent default and asserts the
// same live-plan role codes without consulting a legacy PendingOrder book.
#include <pineforge/source/pine_native_host.hpp>

#include "oracle_fixture_config_shim.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <variant>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::printf("FAIL %d: %s\n", __LINE__, #expr); } } while (false)

class PercentShortSeedProbe final : public source::PineNativeHost {
public:
    PercentShortSeedProbe() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 10.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("Short", false);
        if (bar_index_ == 1) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short");
        }
    }
};

std::optional<no::RequestHandle> latest(const PercentShortSeedProbe& host, const std::string& label) {
    std::optional<no::RequestHandle> out;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* accepted = std::get_if<no::AcceptedEvent>(&*row.command)) {
            if (accepted->request().label == label) out = accepted->handle();
        }
    }
    return out;
}
} // namespace

int main() {
    PercentShortSeedProbe host;
    const Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 1000.0, 1'000},
        {100.0, 100.0, 100.0, 100.0, 1000.0, 2'000},
    };
    host.run(bars, 2);
    CHECK(host.last_error().empty());
    const auto long_entry = latest(host, "Long");
    const auto final_short = latest(host, "Short");
    const auto materialize = latest(host, "__close__Short");
    CHECK(long_entry && final_short && materialize);
    // Formation alone is not role authority; the finite tape ends before the
    // next-open qualification can select and use this plan.
    if (long_entry && final_short && materialize) {
        CHECK(host.short_seed_collision_role_v1(*long_entry) == 0);
        CHECK(host.short_seed_collision_role_v1(*materialize) == 0);
        CHECK(host.short_seed_collision_role_v1(*final_short) == 0);
    }
    std::printf("R4-D native percent ShortSeed roles: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
