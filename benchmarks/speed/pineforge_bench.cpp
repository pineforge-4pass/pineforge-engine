// benchmarks/speed/pineforge_bench.cpp
//
// Google Benchmark target for per-strategy PineForge timing.
//
// ABI NOTE (adapted from plan template):
//   The plan template used placeholder symbols `pf_strategy_run`, `pf_ohlcv`,
//   and `pf_trade_buffer` which do NOT exist in the real C ABI.
//
//   The real ABI (include/pineforge/pineforge.h) uses a multi-step lifecycle:
//     strategy_create(NULL)        → pf_strategy_t handle (opaque void*)
//     run_backtest(s, bars, n, &out) → fills pf_report_t
//     report_free(&out)            → frees heap arrays in report
//     strategy_free(s)             → frees strategy handle
//
//   Bar struct is pf_bar_t {open, high, low, close, volume, timestamp(ms)}.
//   CSV column order: timestamp,open,high,low,close,volume (timestamp is col 0).
//
//   Each strategy .dylib exports these symbols directly (dlopen/dlsym per run).
//
// BENCH_STRATEGIES_DIR and BENCH_OHLCV_PATH are injected by CMake as
// compile-time string macros pointing at the bench-built dylib tree and
// the canonical ETHUSDT_15.csv feed respectively.

#include <benchmark/benchmark.h>
#include <pineforge/pineforge.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ABI function pointer types resolved per-dylib at registration time.
// ---------------------------------------------------------------------------

using fn_strategy_create = pf_strategy_t (*)(const char*);
using fn_run_backtest    = void (*)(pf_strategy_t, pf_bar_t*, int, pf_report_t*);
using fn_run_backtest_full = void (*)(pf_strategy_t, pf_bar_t*, int, const char*, const char*, int, int, pf_magnifier_distribution_t, pf_report_t*);
using fn_report_free     = void (*)(pf_report_t*);
using fn_strategy_free   = void (*)(pf_strategy_t);

// ---------------------------------------------------------------------------
// OHLCV loading — parses the canonical CSV (timestamp,o,h,l,c,v).
// Stored as a flat array of pf_bar_t so it can be passed directly to
// run_backtest without any conversion.
// ---------------------------------------------------------------------------

namespace {

using BarVec = std::vector<pf_bar_t>;

const BarVec& get_bars() {
    static BarVec bars = []() -> BarVec {
        BarVec v;
        FILE* f = std::fopen(BENCH_OHLCV_PATH, "r");
        if (!f) {
            std::fprintf(stderr, "FATAL: cannot open OHLCV CSV: %s\n", BENCH_OHLCV_PATH);
            std::abort();
        }
        char line[256];
        std::fgets(line, sizeof(line), f);  // skip header row
        while (std::fgets(line, sizeof(line), f)) {
            // CSV column order: timestamp,open,high,low,close,volume
            long long ts;
            double op, hi, lo, cl, vo;
            if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf,%lf",
                            &ts, &op, &hi, &lo, &cl, &vo) == 6) {
                pf_bar_t b{};
                b.open      = op;
                b.high      = hi;
                b.low       = lo;
                b.close     = cl;
                b.volume    = vo;
                b.timestamp = static_cast<int64_t>(ts);
                v.push_back(b);
            }
        }
        std::fclose(f);
        return v;
    }();
    return bars;
}

// ---------------------------------------------------------------------------
// Per-strategy benchmark fixture.
//   - dlopen is intentionally done *inside* the benchmark loop body so the
//     loader overhead is included in the wall-clock reading. This matches
//     the intended "cold-load + full backtest" latency definition used by
//     the speed table aggregator (Task 7.3).
//   - If you want hot-loop throughput only, move dlopen outside the loop and
//     add a State::PauseTiming() guard around setup.
// ---------------------------------------------------------------------------

void register_strategy(const std::string& slug, const std::string& dylib_path) {
    // 1. Cold-load (default)
    benchmark::RegisterBenchmark(
        slug.c_str(),
        [dylib_path](benchmark::State& state) {
            const BarVec& bars = get_bars();
            const int     n    = static_cast<int>(bars.size());

            for (auto _ : state) {
                void* h = dlopen(dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (!h) {
                    state.SkipWithError(dlerror());
                    return;
                }

                auto create = reinterpret_cast<fn_strategy_create>(
                    dlsym(h, "strategy_create"));
                auto run = reinterpret_cast<fn_run_backtest>(
                    dlsym(h, "run_backtest"));
                auto rfree = reinterpret_cast<fn_report_free>(
                    dlsym(h, "report_free"));
                auto sfree = reinterpret_cast<fn_strategy_free>(
                    dlsym(h, "strategy_free"));

                if (!create || !run || !rfree || !sfree) {
                    dlclose(h);
                    state.SkipWithError("missing required ABI symbol");
                    return;
                }

                pf_strategy_t s = create(nullptr);
                pf_report_t   report{};
                // Cast away const — bars are read-only but the C API takes
                // a non-const pointer (C ABI has no const on the bar array).
                run(s, const_cast<pf_bar_t*>(bars.data()), n, &report);
                benchmark::DoNotOptimize(report.total_trades);
                rfree(&report);
                sfree(s);
                dlclose(h);
            }
        })
        ->Unit(benchmark::kMicrosecond)
        ->Iterations(20);

    // 2. Hot-loop throughput without magnifier
    benchmark::RegisterBenchmark(
        (slug + "/throughput/no_magnifier").c_str(),
        [dylib_path](benchmark::State& state) {
            const BarVec& bars = get_bars();
            const int     n    = static_cast<int>(bars.size());

            void* h = dlopen(dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) {
                state.SkipWithError(dlerror());
                return;
            }

            auto create = reinterpret_cast<fn_strategy_create>(
                dlsym(h, "strategy_create"));
            auto run = reinterpret_cast<fn_run_backtest>(
                dlsym(h, "run_backtest"));
            auto rfree = reinterpret_cast<fn_report_free>(
                dlsym(h, "report_free"));
            auto sfree = reinterpret_cast<fn_strategy_free>(
                dlsym(h, "strategy_free"));

            if (!create || !run || !rfree || !sfree) {
                dlclose(h);
                state.SkipWithError("missing required ABI symbol");
                return;
            }

            for (auto _ : state) {
                pf_strategy_t s = create(nullptr);
                pf_report_t   report{};
                run(s, const_cast<pf_bar_t*>(bars.data()), n, &report);
                benchmark::DoNotOptimize(report.total_trades);
                rfree(&report);
                sfree(s);
            }
            dlclose(h);

            state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
        })
        ->Unit(benchmark::kMicrosecond)
        ->Iterations(20);

    // 3. Hot-loop throughput with bar magnifier (4 samples, endpoints)
    benchmark::RegisterBenchmark(
        (slug + "/throughput/with_magnifier").c_str(),
        [dylib_path](benchmark::State& state) {
            const BarVec& bars = get_bars();
            const int     n    = static_cast<int>(bars.size());

            void* h = dlopen(dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) {
                state.SkipWithError(dlerror());
                return;
            }

            auto create = reinterpret_cast<fn_strategy_create>(
                dlsym(h, "strategy_create"));
            auto run_full = reinterpret_cast<fn_run_backtest_full>(
                dlsym(h, "run_backtest_full"));
            auto rfree = reinterpret_cast<fn_report_free>(
                dlsym(h, "report_free"));
            auto sfree = reinterpret_cast<fn_strategy_free>(
                dlsym(h, "strategy_free"));

            if (!create || !run_full || !rfree || !sfree) {
                dlclose(h);
                state.SkipWithError("missing required ABI symbol");
                return;
            }

            for (auto _ : state) {
                pf_strategy_t s = create(nullptr);
                pf_report_t   report{};
                run_full(s, const_cast<pf_bar_t*>(bars.data()), n, "", "", 1, 4, PF_MAGNIFIER_ENDPOINTS, &report);
                benchmark::DoNotOptimize(report.total_trades);
                rfree(&report);
                sfree(s);
            }
            dlclose(h);

            state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
        })
        ->Unit(benchmark::kMicrosecond)
        ->Iterations(20);
}

}  // namespace

// ---------------------------------------------------------------------------
// main: enumerate BENCH_STRATEGIES_DIR/<slug>/strategy.dylib (or .so on
// Linux), register one benchmark per found dylib, then hand off to GBench.
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    fs::path root(BENCH_STRATEGIES_DIR);

    for (auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        const auto name = entry.path().filename().string();
        // Skip hidden dirs and _indicator/meta folders that have no dylib.
        if (name.empty() || name[0] == '_' || name[0] == '.') continue;

        // Prefer .dylib (macOS); fall back to .so (Linux).
        fs::path dylib = entry.path() / "strategy.dylib";
        if (!fs::exists(dylib)) dylib = entry.path() / "strategy.so";
        if (!fs::exists(dylib)) continue;  // skip silently (e.g. compile failure)

        register_strategy(name, dylib.string());
    }

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
