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

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <utility>
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
// A slot's pinned strategy() settings: the flat "strategy_overrides" object of
// its inputs.json, which scripts/run_strategy.py applies to the graded run.
// TradingView changed three Pine v6 strategy() defaults on 2026-09-24, after
// every bench tape was recorded, and the codegen declares the new values for
// a script that omits them; the slots that do pin the tape's values. Applying
// the pins here makes the timed run the graded strategy, not a configuration
// no tape has. Values are passed as the JSON writes them: a string without
// its quotes, a number as written (run_strategy.py passes str(value)).
// ---------------------------------------------------------------------------

using Overrides = std::vector<std::pair<std::string, std::string>>;

Overrides read_strategy_overrides(const fs::path& inputs_json) {
    Overrides out;
    FILE* f = std::fopen(inputs_json.c_str(), "r");
    if (!f) return out;
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    size_t i = text.find("\"strategy_overrides\"");
    if (i == std::string::npos) return out;
    i = text.find('{', i);
    const size_t end = i == std::string::npos ? i : text.find('}', i);
    if (end == std::string::npos) return out;
    const std::string body = text.substr(i + 1, end - i - 1);
    if (body.find_first_of("{[") != std::string::npos) {
        std::fprintf(stderr, "FATAL: %s: strategy_overrides is not a flat object\n", inputs_json.c_str());
        std::abort();
    }
    auto skip = [&](size_t& k) { while (k < body.size() && std::isspace(static_cast<unsigned char>(body[k]))) ++k; };
    auto quoted = [&](size_t& k) {  // body[k] == '"'
        const size_t close = body.find('"', k + 1);
        std::string v = body.substr(k + 1, close - k - 1);
        k = close + 1;
        return v;
    };
    for (size_t k = 0;;) {
        skip(k);
        if (k >= body.size()) break;
        std::string key = quoted(k);
        skip(k);
        ++k;  // ':'
        skip(k);
        std::string value;
        if (body[k] == '"') {
            value = quoted(k);
        } else {
            const size_t v0 = k;
            while (k < body.size() && body[k] != ',' && !std::isspace(static_cast<unsigned char>(body[k]))) ++k;
            value = body.substr(v0, k - v0);
        }
        out.emplace_back(std::move(key), std::move(value));
        skip(k);
        if (k < body.size() && body[k] == ',') ++k;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The C ABI of one strategy library, resolved once per dlopen.
// ---------------------------------------------------------------------------

using fn_strategy_set_override = void (*)(pf_strategy_t, const char*, const char*);

struct Api {
    fn_strategy_create create = nullptr;
    fn_run_backtest run = nullptr;
    fn_run_backtest_full run_full = nullptr;
    fn_report_free rfree = nullptr;
    fn_strategy_free sfree = nullptr;
    fn_strategy_set_override set_override = nullptr;

    explicit Api(void* h)
        : create(reinterpret_cast<fn_strategy_create>(dlsym(h, "strategy_create"))),
          run(reinterpret_cast<fn_run_backtest>(dlsym(h, "run_backtest"))),
          run_full(reinterpret_cast<fn_run_backtest_full>(dlsym(h, "run_backtest_full"))),
          rfree(reinterpret_cast<fn_report_free>(dlsym(h, "report_free"))),
          sfree(reinterpret_cast<fn_strategy_free>(dlsym(h, "strategy_free"))),
          set_override(reinterpret_cast<fn_strategy_set_override>(dlsym(h, "strategy_set_override"))) {}

    // The symbols a benchmark needs, or the reason it cannot run.
    const char* missing(bool full, const Overrides& overrides) const {
        if (!create || !(full ? run_full != nullptr : run != nullptr) || !rfree || !sfree)
            return "missing required ABI symbol";
        if (!overrides.empty() && !set_override)
            return "inputs.json pins strategy_overrides but the library has no strategy_set_override";
        return nullptr;
    }

    pf_strategy_t create_pinned(const Overrides& overrides) const {
        pf_strategy_t s = create(nullptr);
        for (const auto& [key, value] : overrides) set_override(s, key.c_str(), value.c_str());
        return s;
    }
};

// ---------------------------------------------------------------------------
// Per-strategy benchmark fixture.
//   - The cold-load benchmark dlopens *inside* the loop, so the loader
//     overhead is in the wall-clock reading ("cold-load + full backtest").
//   - The throughput benchmarks dlopen once outside the loop; each timed
//     iteration is strategy_create (+ the slot's pins) and one backtest over
//     the whole feed.
//   - Each benchmark reports the last iteration's closed-trade count as its
//     "trades" counter: the workload the time was measured on.
// ---------------------------------------------------------------------------

void register_strategy(const std::string& slug, const std::string& dylib_path,
                       const Overrides& overrides) {
    // 1. Cold-load (default)
    benchmark::RegisterBenchmark(
        slug.c_str(),
        [dylib_path, overrides](benchmark::State& state) {
            const BarVec& bars = get_bars();
            const int     n    = static_cast<int>(bars.size());
            int trades = 0;

            for (auto _ : state) {
                void* h = dlopen(dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (!h) {
                    state.SkipWithError(dlerror());
                    return;
                }
                const Api api(h);
                if (const char* why = api.missing(false, overrides)) {
                    dlclose(h);
                    state.SkipWithError(why);
                    return;
                }

                pf_strategy_t s = api.create_pinned(overrides);
                pf_report_t   report{};
                // Cast away const — bars are read-only but the C API takes
                // a non-const pointer (C ABI has no const on the bar array).
                api.run(s, const_cast<pf_bar_t*>(bars.data()), n, &report);
                benchmark::DoNotOptimize(report.total_trades);
                trades = report.total_trades;
                api.rfree(&report);
                api.sfree(s);
                dlclose(h);
            }
            state.counters["trades"] = trades;
        })
        ->Unit(benchmark::kMicrosecond)
        ->Iterations(20);

    // 2. Hot-loop throughput without magnifier, 3. with the bar magnifier
    //    (4 samples, endpoints)
    for (const bool magnifier : {false, true}) {
        benchmark::RegisterBenchmark(
            (slug + (magnifier ? "/throughput/with_magnifier" : "/throughput/no_magnifier")).c_str(),
            [dylib_path, overrides, magnifier](benchmark::State& state) {
                const BarVec& bars = get_bars();
                const int     n    = static_cast<int>(bars.size());

                void* h = dlopen(dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (!h) {
                    state.SkipWithError(dlerror());
                    return;
                }
                const Api api(h);
                if (const char* why = api.missing(magnifier, overrides)) {
                    dlclose(h);
                    state.SkipWithError(why);
                    return;
                }

                int trades = 0;
                for (auto _ : state) {
                    pf_strategy_t s = api.create_pinned(overrides);
                    pf_report_t   report{};
                    if (magnifier) {
                        api.run_full(s, const_cast<pf_bar_t*>(bars.data()), n, "", "", 1, 4,
                                     PF_MAGNIFIER_ENDPOINTS, &report);
                    } else {
                        api.run(s, const_cast<pf_bar_t*>(bars.data()), n, &report);
                    }
                    benchmark::DoNotOptimize(report.total_trades);
                    trades = report.total_trades;
                    api.rfree(&report);
                    api.sfree(s);
                }
                dlclose(h);

                state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
                state.counters["trades"] = trades;
            })
            ->Unit(benchmark::kMicrosecond)
            ->Iterations(20);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// main: enumerate BENCH_STRATEGIES_DIR/<slug>/strategy.dylib (or .so on
// Linux) and, when it exists, BENCH_CLOSED_STRATEGIES_DIR likewise; register
// one benchmark per found dylib, then hand off to GBench.
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // The public slots, then the maintainer-local closed slots when present.
    // A closed slot registers as "NNN-closed", as every report prints it
    // (benchmarks/paths.py public_name): its directory name carries the
    // TradingView author's handle, which no committed timing file repeats.
    const fs::path closed_root(BENCH_CLOSED_STRATEGIES_DIR);
    int pinned = 0;
    for (const fs::path root : {fs::path(BENCH_STRATEGIES_DIR), closed_root}) {
        if (!fs::is_directory(root)) continue;
        for (auto& entry : fs::directory_iterator(root)) {
            if (!entry.is_directory()) continue;
            const auto name = entry.path().filename().string();
            // Skip hidden dirs and _indicator/meta folders that have no dylib.
            if (name.empty() || name[0] == '_' || name[0] == '.') continue;

            // Prefer .dylib (macOS); fall back to .so (Linux).
            fs::path dylib = entry.path() / "strategy.dylib";
            if (!fs::exists(dylib)) dylib = entry.path() / "strategy.so";
            if (!fs::exists(dylib)) continue;  // skip silently (e.g. compile failure)

            const Overrides overrides = read_strategy_overrides(entry.path() / "inputs.json");
            pinned += !overrides.empty();
            register_strategy(root == closed_root ? name.substr(0, 3) + "-closed" : name,
                              dylib.string(), overrides);
        }
    }
    std::fprintf(stderr, "pineforge_bench: %d slots pin strategy_overrides from inputs.json\n", pinned);

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
