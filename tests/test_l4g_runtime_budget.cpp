// L4g: keep the switched route within the verifier's per-strategy budget.
//
// This deliberately drives the shipped tutorial MACD through a long,
// timestamp-monotone replay and adds re-issued protective brackets while a
// tutorial position is live. It exercises the ordinary source adapter route
// (cohort resolution, request replacement, and the native O/H/L/C driver)
// without needing corpus data in the unit-test checkout.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "../tutorial/macd/generated.cpp"

namespace {

int failures = 0;

#define CHECK(condition) do {                                                     \
    if (!(condition)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                               \
    }                                                                             \
} while (0)

std::vector<pineforge::Bar> load_tutorial_bars() {
    std::ifstream input(PINEFORGE_L4G_TUTORIAL_CSV);
    if (!input) {
        std::fprintf(stderr, "FAIL cannot open tutorial tape %s\n",
                     PINEFORGE_L4G_TUTORIAL_CSV);
        ++failures;
        return {};
    }
    std::string line;
    std::getline(input, line);  // CSV header
    std::vector<pineforge::Bar> bars;
    while (std::getline(input, line)) {
        std::stringstream row(line);
        std::string field;
        std::vector<std::string> fields;
        while (std::getline(row, field, ',')) fields.push_back(field);
        if (fields.size() != 6) {
            std::fprintf(stderr, "FAIL malformed tutorial bar: %s\n", line.c_str());
            ++failures;
            return {};
        }
        bars.push_back({std::stod(fields[1]), std::stod(fields[2]), std::stod(fields[3]),
                        std::stod(fields[4]), std::stod(fields[5]), std::stoll(fields[0])});
    }
    return bars;
}

std::vector<pineforge::Bar> repeat_tutorial_tape(
        const std::vector<pineforge::Bar>& source) {
    constexpr int kRepeats = 64;
    std::vector<pineforge::Bar> result;
    if (source.empty()) return result;
    result.reserve(source.size() * kRepeats);
    const std::int64_t step = source.size() > 1
        ? source[1].timestamp - source[0].timestamp : 900000;
    const std::int64_t span = source.back().timestamp - source.front().timestamp + step;
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        const std::int64_t offset = static_cast<std::int64_t>(repeat) * span;
        for (const pineforge::Bar& bar : source) {
            pineforge::Bar copy = bar;
            copy.timestamp += offset;
            result.push_back(copy);
        }
    }
    return result;
}

class TutorialBracketReplay final : public GeneratedStrategy {
public:
    void on_source_bar(const pineforge::Bar& bar) override {
        GeneratedStrategy::on_source_bar(bar);
        const double position = signed_position_size();
        if (position > 0.0) {
            strategy_exit("L4g tutorial long guard", "Long", bar.close * 1.60,
                          bar.close * 0.40);
        } else if (position < 0.0) {
            strategy_exit("L4g tutorial short guard", "Short", bar.close * 0.40,
                          bar.close * 1.60);
        }
    }
};

} // namespace

int main() {
    const auto seed = load_tutorial_bars();
    const auto bars = repeat_tutorial_tape(seed);
    CHECK(!bars.empty());
    CHECK(bars.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()));
    if (bars.empty() || bars.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return 1;

    TutorialBracketReplay strategy;
    const auto started = std::chrono::steady_clock::now();
    strategy.run(bars.data(), static_cast<int>(bars.size()));
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    CHECK(strategy.last_error().empty());
    CHECK(strategy.script_bars_processed() == static_cast<std::int64_t>(bars.size()));
    // Captured on the shared 16-core host at c71699f: the repaired route is
    // below one second. Leave deterministic CI headroom while still catching
    // the pre-fix multi-minute history scan.
    if (elapsed > 12.0) {
        std::fprintf(stderr,
                     "FAIL tutorial reissue runtime %.3fs exceeds 12.000s (%zu bars)\n",
                     elapsed, bars.size());
        ++failures;
    }
    std::printf("L4g tutorial reissue runtime %.3fs over %zu bars\n", elapsed, bars.size());
    return failures == 0 ? 0 : 1;
}
