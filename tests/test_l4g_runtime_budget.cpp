// A30/L8d: one source TU is compiled against ab9714be and the current engine.
// The runner compares those two executables; this binary reports one sample.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(condition) do {                                                     \
    if (!(condition)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                               \
    }                                                                             \
} while (0)

std::vector<pineforge::Bar> load_bars() {
    std::ifstream input(PINEFORGE_L4G_TUTORIAL_CSV);
    if (!input) return {};
    std::string line;
    std::getline(input, line);
    std::vector<pineforge::Bar> bars;
    while (std::getline(input, line)) {
        std::stringstream row(line);
        std::string field;
        std::vector<std::string> fields;
        while (std::getline(row, field, ',')) fields.push_back(field);
        if (fields.size() != 6) return {};
        bars.push_back({std::stod(fields[1]), std::stod(fields[2]),
                        std::stod(fields[3]), std::stod(fields[4]),
                        std::stod(fields[5]), std::stoll(fields[0])});
    }
    return bars;
}

std::vector<pineforge::Bar> repeat(const std::vector<pineforge::Bar>& source) {
    constexpr int kRepeats = 64;
    std::vector<pineforge::Bar> result;
    if (source.empty()) return result;
    result.reserve(source.size() * kRepeats);
    const std::int64_t step = source.size() > 1
        ? source[1].timestamp - source[0].timestamp : 900000;
    const std::int64_t span = source.back().timestamp - source.front().timestamp + step;
    for (int iteration = 0; iteration < kRepeats; ++iteration) {
        const std::int64_t offset = static_cast<std::int64_t>(iteration) * span;
        for (const auto& bar : source) {
            auto copy = bar;
            copy.timestamp += offset;
            result.push_back(copy);
        }
    }
    return result;
}

class ReissueReplay final : public pineforge::source::PineStrategyHost {
public:
    std::int64_t callbacks = 0;
    void on_source_bar(const pineforge::Bar& bar) override {
        ++callbacks;
        const double absent = std::numeric_limits<double>::quiet_NaN();
        if (pine_bar_index() == 0)
            strategy_entry("L", true, absent, absent, 1.0);
        if (live_position_size() > 0.0)
            strategy_exit("guard", "L", bar.close * 1.60, bar.close * 0.40);
    }
};

} // namespace

int main() {
    const auto bars = repeat(load_bars());
    CHECK(!bars.empty());
    CHECK(bars.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()));
    if (bars.empty() || bars.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return 1;
    ReissueReplay strategy;
    const auto started = std::chrono::steady_clock::now();
    strategy.run(bars.data(), static_cast<int>(bars.size()));
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    CHECK(strategy.last_error().empty());
    CHECK(strategy.callbacks == static_cast<std::int64_t>(bars.size()));
    CHECK(std::isfinite(strategy.live_position_size()));
    CHECK(strategy.broker_state_hash() != 0);
    std::printf("PF_RUNTIME_SECONDS=%.9f\n", elapsed);
    std::printf("runtime replay callbacks=%lld bars=%zu\n",
                static_cast<long long>(strategy.callbacks), bars.size());
    return failures == 0 ? 0 : 1;
}
