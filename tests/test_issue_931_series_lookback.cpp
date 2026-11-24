#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_ta_length.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

// Keep the red comparison buildable against the issue's base headers.
#ifdef PINEFORGE_HAS_TA_LOOKBACK_LIMIT_V1
namespace {

class CallbackHost final : public pineforge::source::PineStrategyHost {
public:
    CallbackHost() {
        attach_pine_execution_adapter();
        configure_pine_strategy({});
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        ++entered;
        pineforge::source::SeriesSma sma;
        if (std::isnan(sma.compute(bar.close, 15002))) ++warmed;
    }

    int entered = 0;
    int warmed = 0;
};

int run_cap_control() {
    const auto setter = &pf_ta_lookback_limit_set_checked;
    const std::int64_t tv_cap = 15000;
    const std::int64_t invalid_cap = -1;
    std::array<char, 128> error{};
    if (const int status = setter(&tv_cap, error.data(), error.size()); status != PF_SETTINGS_OK) {
        std::fprintf(stderr, "NOT COMPARED: setting cap 15000: expected status %d; actual %d (%s)\n",
                     PF_SETTINGS_OK, status, error.data());
        return 2;
    }
    if (const int status = setter(&invalid_cap, error.data(), error.size());
            status != PF_SETTINGS_INVALID_ARGUMENT) {
        std::fprintf(stderr, "FAIL: setting cap -1: expected status %d; actual %d\n",
                     PF_SETTINGS_INVALID_ARGUMENT, status);
        return 1;
    }
    CallbackHost capped;
    if (const int status = setter(nullptr, error.data(), error.size()); status != PF_SETTINGS_OK) {
        std::fprintf(stderr, "NOT COMPARED: clearing cap: expected status %d; actual %d (%s)\n",
                     PF_SETTINGS_OK, status, error.data());
        return 2;
    }
    CallbackHost unlimited;
    const pineforge::Bar bars[] = {
        {1, 1, 1, 1, 1, 1704067200000},
        {1, 1, 1, 1, 1, 1704067260000},
    };
    // Both hosts execute after the creation scope is cleared. A call site
    // created inside the callback still sees the capped handle's policy.
    capped.run(bars, 2, "1", "1");
    unlimited.run(bars, 2, "1", "1");
    auto* capped_handle = static_cast<pineforge::BacktestEngine*>(&capped);
    auto* unlimited_handle = static_cast<pineforge::BacktestEngine*>(&unlimited);
    if (capped.entered == 0 || unlimited.entered == 0) {
        std::fprintf(stderr, "NOT COMPARED: callbacks: expected capped 1 and unlimited 2; actual %d and %d\n",
                     capped.entered, unlimited.entered);
        return 2;
    }
    const char* code = strategy_get_last_error_code(capped_handle);
    const char* args = strategy_get_last_error_args(capped_handle);
    constexpr std::string_view expected_args = R"({"limit":"max_bars_back","max":15000})";
    if (capped.entered != 1 || capped.warmed != 0 || strategy_last_run_status(capped_handle) != 1
        || !code || std::string_view(code) != "pine_runtime_limit"
        || !args || std::string_view(args) != expected_args) {
        std::fprintf(stderr, "FAIL: capped callback: expected 1 entered, 0 warmed, status 1 and pine_runtime_limit %s; actual %d entered, %d warmed, status %d and %s %s\n",
                     expected_args.data(), capped.entered, capped.warmed,
                     strategy_last_run_status(capped_handle), code ? code : "(null)",
                     args ? args : "(null)");
        return 1;
    }
    if (unlimited.entered != 2 || unlimited.warmed != 2
        || strategy_last_run_status(unlimited_handle) != 0) {
        std::fprintf(stderr, "FAIL: unlimited callback: expected 2 entered, 2 warmed, status 0; actual %d entered, %d warmed, status %d\n",
                     unlimited.entered, unlimited.warmed, strategy_last_run_status(unlimited_handle));
        return 1;
    }
    return 0;
}

} // namespace
#endif

int main(int argc, char** argv) {
    constexpr int length = 15002;
    constexpr double source = 1.0;
    std::vector<double> samples(length, source);
    // Negative controls for the lab: neither input may reach a comparison.
    if (argc == 2 && std::string_view(argv[1]) == "--empty") {
        samples.clear();
    } else if (argc == 2 && std::string_view(argv[1]) == "--wrong-input") {
        samples.front() = 2.0;
    } else if (argc > 1) {
        std::fprintf(stderr, "NOT COMPARED: arguments: expected none, --empty or --wrong-input; got %s (%d arguments)\n",
                     argv[1], argc - 1);
        return 2;
    }
    if (samples.size() != length) {
        std::fprintf(stderr, "NOT COMPARED: issue sample count: expected %d; actual %zu\n",
                     length, samples.size());
        return 2;
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i] == source) continue;
        std::fprintf(stderr, "NOT COMPARED: issue sample %zu: expected %.17g; actual %.17g\n",
                     i, source, samples[i]);
        return 2;
    }

    pineforge::source::SeriesSma sma;
    const char* step = "first SeriesSma::compute(1, 15002): expected warm-up na";
    try {
        const double warmup = sma.compute(samples.front(), length);
        if (!std::isnan(warmup)) {
            std::fprintf(stderr, "FAIL: %s; actual %.17g\n", step, warmup);
            return 1;
        }
        step = "extend the issue history through sample 15001";
        for (std::size_t i = 1; i + 1 < samples.size(); ++i)
            (void)sma.compute(samples[i], 1);

        step = "full SeriesSma::compute(1, 15002): expected 1";
        const double mean = sma.compute(samples.back(), length);
        if (mean != source) {
            std::fprintf(stderr, "FAIL: %s; actual %.17g\n", step, mean);
            return 1;
        }

        // This must average the old samples, even though the newest one is zero.
        step = "SeriesSma::recompute(0, 15002) after the full window";
        const double revised = sma.recompute(0.0, length);
        const double expected = (length - 1.0) / length;
        if (revised != expected) {
            std::fprintf(stderr, "FAIL: %s: expected %.17g; actual %.17g\n",
                         step, expected, revised);
            return 1;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s; actual %s\n", step, error.what());
        return 1;
    }

#ifdef PINEFORGE_HAS_TA_LOOKBACK_LIMIT_V1
    try {
        if (const int status = run_cap_control(); status != 0) return status;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NOT COMPARED: callback setup: expected two runnable hosts; actual %s\n",
                     error.what());
        return 2;
    }
#else
    std::fputs("NOT COMPARED: lookback setter: expected available; actual absent\n", stderr);
    return 2;
#endif
    std::puts("PASS: series SMA uses all 15002 samples with no lookback limit");
    return 0;
}
