// Keeps the source-host admission projections reachable after the legacy book
// removal. The old direct Book fixture is covered by this public route probe.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)
Bar flat(double price, std::int64_t timestamp) { return {price, price, price, price, 1, timestamp}; }
class AdmissionProbe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("A", true, na<double>(), na<double>(), 1.0);
    }
};
}

int main() {
    const std::vector<Bar> bars = {flat(100, 0), flat(100, 60'000)};
    AdmissionProbe probe;
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(!probe.market_admission_journal().events().empty());
    CHECK(!probe.market_admission_fields().empty());
    std::printf("native market-admission projection: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
