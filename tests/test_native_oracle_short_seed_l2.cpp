// Native-route ShortSeed role-lifetime witness for the L0 finding-272 command
// shape. A plan that has not reached and been used at its qualifying broker
// open is not allowed to project executable role codes.
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

class ShortSeedProbe final : public source::PineNativeHost {
public:
    ShortSeedProbe() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false);
        } else if (bar_index_ == 1) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short");
        }
    }
};

std::optional<no::RequestHandle> latest_label(const ShortSeedProbe& host, const std::string& label) {
    std::optional<no::RequestHandle> result;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* accepted = std::get_if<no::AcceptedEvent>(&*row.command)) {
            if (accepted->request().label == label) result = accepted->handle();
        }
    }
    return result;
}
} // namespace

int main() {
    ShortSeedProbe host;
    const Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 1000.0, 1'000},
        {100.0, 100.0, 100.0, 100.0, 1000.0, 2'000},
    };
    host.fixture_retain_all_events();  // read after the run (V19-B)
    host.run(bars, 2);
    CHECK(host.last_error().empty());
    const auto long_entry = latest_label(host, "Long");
    const auto final_short = latest_label(host, "Short");
    const auto materialize = latest_label(host, "__close__Short");
    CHECK(long_entry.has_value());
    CHECK(final_short.has_value());
    CHECK(materialize.has_value());
    if (long_entry && final_short && materialize) {
        CHECK(host.short_seed_collision_role_v1(*long_entry) == 0);
        CHECK(host.short_seed_collision_role_v1(*materialize) == 0);
        CHECK(host.short_seed_collision_role_v1(*final_short) == 0);
    }
    const auto unrelated = latest_label(host, "__close__Long");
    if (unrelated) CHECK(host.short_seed_collision_role_v1(*unrelated) == 0);
    std::printf("R4-D native ShortSeed roles: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
