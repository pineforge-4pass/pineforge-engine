// Native-route twin of tests/oracle/test_oracle_magnifier_barstate.cpp.
#include <pineforge/source/pine_native_host.hpp>

#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

class CadenceProbe final : public source::PineNativeHost {
public:
    std::vector<bool> first;
    std::vector<bool> confirmed;
    std::vector<bool> last;
    std::vector<bool> advances_history;
    void on_source_bar(const Bar&) override {
        first.push_back(is_first_tick());
        confirmed.push_back(is_last_tick());
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
    probe.run(bars, 4, "1", "2", true, 4, MagnifierDistribution::ENDPOINTS);
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
} // namespace

int main() {
    check_distribution(false);
    check_distribution(true);
    std::printf("R4-D native magnifier cadence twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
