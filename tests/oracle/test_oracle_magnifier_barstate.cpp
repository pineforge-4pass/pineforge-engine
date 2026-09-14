/*
 * R4-D L0 literal legacy-route oracle for terminal-sub-bar script cadence.
 * Captured at ab9714beccb62b796c122cf68986ec9e7dbf4a67.  The corpus manifest
 * has ENDPOINTS and the volume-weighted magnifier lane; both are frozen here.
 */

#include <cstdio>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/magnifier.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

class CadenceProbe final : public source::PineStrategyHost {
public:
    std::vector<bool> first;
    std::vector<bool> confirmed;
    std::vector<bool> last;
    std::vector<bool> advances_history;

    void on_source_bar(const Bar&) override {
        first.push_back(is_first_tick_);
        confirmed.push_back(is_last_tick_);
        last.push_back(barstate_islast_);
        advances_history.push_back(history_advances_new_bar());
    }
};

void check_distribution(bool volume_weighted) {
    CadenceProbe probe;
    if (volume_weighted) probe.set_magnifier_volume_weighted(true);
    const Bar bars[] = {
        {100, 101, 99, 100, 10,  60'000},
        {101, 102,100, 101, 20, 120'000},
        {102, 103,101, 102, 30, 180'000},
        {103, 104,102, 103, 40, 240'000},
    };
    probe.run(bars, 4, "1", "2", /*bar_magnifier=*/true, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    CHECK(probe.first.size() == 2);
    CHECK(probe.confirmed.size() == 2);
    CHECK(probe.last.size() == 2);
    CHECK(probe.advances_history.size() == 2);
    if (probe.first.size() == 2) {
        CHECK(probe.first[0] && probe.first[1]);
        CHECK(probe.confirmed[0] && probe.confirmed[1]);
        CHECK(!probe.last[0] && probe.last[1]);
        CHECK(probe.advances_history[0] && probe.advances_history[1]);
    }
}
}  // namespace

int main() {
    check_distribution(false);  // validation/magnifier-tick-dist-endpoints-* lanes
    check_distribution(true);   // validation/magnifier-tick-dist-volume-weighted-on-01
    std::printf("R4-D magnifier cadence oracle: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
