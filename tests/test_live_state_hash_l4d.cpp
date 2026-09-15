// A29 native-route twin for test_live_state_hash.cpp.
//
// The base mutation census reached into every member of the retired source
// owner.  This replacement keeps its two executable CHECK literals focused on
// what survives the switch: equal public command histories hash equally, and
// a different accepted native command changes the live broker hash.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <limits>

using namespace pineforge;

namespace {

int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

class Probe final : public pineforge::source::PineStrategyHost {
public:
    explicit Probe(bool extra) : extra_(extra) {
        initial_capital_ = 10'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 2;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (extra_ && bar_index_ == 1) {
            const double missing = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("A", true, missing, missing, 1.0);
        }
    }

private:
    bool extra_;
};

}  // namespace

int main() {
    const Bar bars[] = {
        {100, 101, 99, 100, 1, 0},
        {101, 102, 100, 101, 1, 60'000},
        {102, 103, 101, 102, 1, 120'000},
    };
    Probe first(false), second(false), changed(true);
    first.run(bars, 3);
    second.run(bars, 3);
    changed.run(bars, 3);
    CHECK(first.broker_state_hash() == second.broker_state_hash());
    CHECK(first.broker_state_hash() != changed.broker_state_hash());
    return failures == 0 ? 0 : 1;
}

#undef CHECK
#undef PineStrategyHost
