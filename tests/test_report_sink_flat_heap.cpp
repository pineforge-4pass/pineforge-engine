// CR-150 lane L0, acceptance (a'), DESIGN.md section 7.
//
// RED ON 3fc010a8b: this file does not compile. It includes
// <pineforge/report_sink.h>, which lane L6b-1 creates, and configures a sink
// through strategy_set_report_sink_v1. The first diagnostic is the recorded
// fail-before (engine/AGENTS.md and engine/CLAUDE.md require it for a new test
// unit).
//
// What it asserts once the sink exists:
//   The engine keeps O(1) live heap with a sink installed. Measured with
//   tests/global_allocation_replacement.hpp in bytes mode (the pattern of
//   tests/test_native_batch_log_presize.cpp:307-321): live bytes after
//   run_backtest_full returns, before strategy_free.
//     bars axis:   16,500 -> 66,000 bars              growth <= 64 KiB
//     trades axis: sparse (~10 trades) -> dense (>1,000) at 66,000 bars
//                                                     growth <= 64 KiB
//   The module is the tutorial MACD generated.cpp linked into this executable
//   (the release-shaped recipe), driven through the real C ABI. No Python and
//   no fake engine.

#define PINEFORGE_TEST_ALLOCATION_BYTES 1
#include "global_allocation_replacement.hpp"

#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        ++checks;                                                                 \
        if (!(condition)) {                                                       \
            ++failures;                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);       \
        }                                                                         \
    } while (0)

extern "C" {
void* strategy_create(const char* params_json);
void run_backtest_full(void* strategy, pf_bar_t* bars, int n, const char* input_tf,
                       const char* script_tf, int bar_magnifier, int magnifier_samples,
                       int magnifier_dist, pf_report_t* out);
void strategy_free(void* strategy);
void report_free(pf_report_t* report);
int strategy_set_report_sink_v1(pf_strategy_t strategy, const pf_report_sink_v1* sink);
}

constexpr int kSparseBars = 16500;
constexpr int kDenseBars = 66000;
constexpr std::size_t kGrowthLimit = 64u * 1024u;

std::string temp_path() {
    char name[] = "/tmp/pf_flat_heap_XXXXXX";
    const int fd = mkstemp(name);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
    return name;
}

// Two shapes that differ only in how often the MACD crosses:
//   smooth: a triangle over 20,000 bars, about ten crosses in 66,000 bars;
//   choppy: price alternates every bar, thousands of crosses.
std::vector<pf_bar_t> bars(int n, bool choppy) {
    std::vector<pf_bar_t> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double price = 0.0;
        if (choppy) {
            price = 100.0 + ((i / 3) % 2 == 0 ? 1.0 : -1.0);
        } else {
            const int phase = i % 20000;
            const int triangle = phase < 10000 ? phase : 20000 - phase;
            price = 100.0 + 0.0005 * static_cast<double>(triangle);
        }
        out.push_back({price, price + 0.5, price - 0.5, price, 1000.0,
                       1700000000000LL + static_cast<std::int64_t>(i) * 60000LL});
    }
    return out;
}

struct Measurement {
    std::size_t live_bytes = 0;
    int total_trades = 0;
    std::int64_t script_bars = 0;
};

// One real run with a sink, measured over the engine's own allocations.
Measurement run(const std::vector<pf_bar_t>& feed) {
    const std::string sink_path = temp_path();
    void* strategy = strategy_create(nullptr);
    CHECK(strategy != nullptr);

    pf_report_sink_v1 sink{};
    sink.struct_size = sizeof(sink);
    sink.version = 1;
    sink.path = sink_path.c_str();
    sink.run_id = "l0-flat-heap";
    sink.scratch_dir = nullptr;
    const int accepted = strategy_set_report_sink_v1(strategy, &sink);
    CHECK(accepted == 0);

    pf_report_t report{};
    global_allocation::live_bytes = 0;
    global_allocation::recording = true;
    run_backtest_full(strategy, const_cast<pf_bar_t*>(feed.data()),
                      static_cast<int>(feed.size()), "1", "1", 0, 4, 0, &report);
    global_allocation::recording = false;

    Measurement measured;
    measured.live_bytes = global_allocation::live_bytes;
    measured.total_trades = report.total_trades;
    measured.script_bars = report.script_bars_processed;

    // DESIGN.md 6: with a sink the report carries the summary only.
    CHECK(report.trades == nullptr);
    CHECK(report.trades_len == 0);
    CHECK(report.equity_curve == nullptr);
    CHECK(report.equity_curve_len == 0);

    report_free(&report);
    strategy_free(strategy);
    std::remove(sink_path.c_str());
    return measured;
}

void bars_axis_is_flat() {
    const Measurement small = run(bars(kSparseBars, false));
    const Measurement large = run(bars(kDenseBars, false));
    const std::size_t grew = large.live_bytes > small.live_bytes
                                 ? large.live_bytes - small.live_bytes
                                 : 0;
    std::printf("bars: %d -> %d: live %zu -> %zu bytes (%zu grew), %d / %d trades\n",
                kSparseBars, kDenseBars, small.live_bytes, large.live_bytes, grew,
                small.total_trades, large.total_trades);
    CHECK(small.script_bars == kSparseBars);
    CHECK(large.script_bars == kDenseBars);
    CHECK(grew <= kGrowthLimit);
}

void trades_axis_is_flat() {
    const Measurement sparse = run(bars(kDenseBars, false));
    const Measurement dense = run(bars(kDenseBars, true));
    const std::size_t grew =
        dense.live_bytes > sparse.live_bytes ? dense.live_bytes - sparse.live_bytes : 0;
    std::printf("trades: %d -> %d: live %zu -> %zu bytes (%zu grew), %d / %d trades\n",
                sparse.total_trades, dense.total_trades, sparse.live_bytes,
                dense.live_bytes, grew, sparse.total_trades, dense.total_trades);
    // The axis is only exercised if the two runs really differ this much.
    CHECK(sparse.total_trades >= 2);
    CHECK(dense.total_trades >= 1000);
    CHECK(grew <= kGrowthLimit);
}

}  // namespace

int main() {
    bars_axis_is_flat();
    trades_axis_is_flat();
    std::printf("report-sink flat heap: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
