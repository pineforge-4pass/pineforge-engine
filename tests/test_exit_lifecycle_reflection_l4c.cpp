#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"

#include <pineforge/pending_order_mirror.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace pineforge;

namespace {
int checks = 0;
int failed = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)
constexpr double na = std::numeric_limits<double>::quiet_NaN();

class ProjectionProbe final : public pineforge::source::PineStrategyHost {
public:
    ProjectionProbe() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", true, na, na, 1);
        if (bar_index_ == 1) {
            strategy_exit("X", "E", 110, 95, na, na, na, 100, "reflection");
            const auto& view = pending_intent_view();
            for (int index = 0; index < view.size(); ++index) {
                pf_pending_order_v1_t candidate{};
                if (view.copy_v1(index, &candidate) == 0
                    && std::strcmp(candidate.id, "X") == 0
                    && std::isfinite(candidate.stop_price)) {
                    mirror = candidate;
                    break;
                }
            }
        }
    }
    pf_pending_order_v1_t mirror{};
};

void reflection_contract() {
    ProjectionProbe probe;
    const Bar bars[] = {{100,100,100,100,1,0}, {100,100,100,100,1,60000},
                        {100,111,94,100,1,120000}};
    probe.run(bars, 3);
    const auto& row = probe.mirror;
    CHECK(probe.last_error().empty());
    CHECK(row.struct_version == PF_PENDING_ORDER_STRUCT_VERSION
          && row.size == sizeof(pf_pending_order_v1_t));
    CHECK(std::strcmp(row.id, "X") == 0);
    CHECK(std::strcmp(row.from_entry, "E") == 0);
    CHECK(row.type == 2);
    CHECK(row.is_long == 0);
    CHECK(std::abs(row.limit_price - 110) < 1e-9);
    CHECK(std::abs(row.stop_price - 95) < 1e-9);
    CHECK(row.birth_cause == static_cast<int>(OrderBirthCause::ChartEvaluation));
    CHECK(row.legs_definition_value_present == 1);
    CHECK(std::abs(row.legs_definition_stop_price - 95) < 1e-9);
}
} // namespace

int main() {
    reflection_contract();
    std::printf("canonical reflection: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
