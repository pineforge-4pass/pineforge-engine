// Observation only: write the engine's own close-of-bar publications.
#include <pineforge/native_host.hpp>

#if defined(PF_GRID_NATIVE_MODULE) || defined(PF_GRID_PINE_MODULE)
#include <pineforge/native_module.hpp>
#endif
#if defined(PF_GRID_PINE_MODULE)
#include <pineforge/source/pine_strategy_host.hpp>
#endif

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

#define PF_GRID_STRINGIFY1(x) #x
#define PF_GRID_STRINGIFY(x) PF_GRID_STRINGIFY1(x)
#ifndef PF_GRID_BUILD
#define PF_GRID_BUILD ctest
#endif

void created(const char* adapter, const BacktestEngine& host) {
    std::cerr << "GRID CREATE " << PF_GRID_STRINGIFY(PF_GRID_BUILD) << ' '
              << adapter << ' ' << host.execution_contract() << '\n';
}

void publish(const BacktestEngine& host, std::int64_t label, const Bar& bar) {
    std::cerr.precision(std::numeric_limits<double>::max_digits10);
    std::cerr << "GRID BAR " << (host.stream_is_realtime() ? "live" : "history") << ' '
              << label << ' ' << bar.open << ' ' << bar.high << ' ' << bar.low << ' '
              << bar.close << ' ' << bar.volume << '\n';
}

void opened(const BacktestEngine& host, const NativeRunSpec* spec) {
    if (!spec) return;
    std::cerr << "GRID CLOCK " << spec->input_tf << ' ' << spec->script_tf << ' '
              << spec->timezone << ' ' << spec->session << ' '
              << host.execution_contract() << '\n';
}

#if defined(PF_GRID_PINE_MODULE)
// Same source-adapter lifecycle as runner/examples/strategy.cpp. The loaded
// engine owns stream_begin/push_tick/advance_time; the body only observes.
class GridPine final : public source::PineStrategyHost {
public:
    GridPine() {
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_value = 1;
        configure_pine_strategy(config);
        created("pine", *this);
    }
    void on_source_bar(const Bar& bar) override {
        if (!seen_bar_) {
            opened(*this, native_state().spec);
            seen_bar_ = true;
        }
        publish(*this, bar.timestamp, bar);
    }
private:
    bool seen_bar_ = false;
};
#else
class GridNative : public NativeStrategyHost {
public:
    GridNative() { created("native", *this); }
    void on_native_run_begin() override { opened(*this, native_state().spec); }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        publish(*this, context.script_bar_open_ms, bar);
    }
    std::int64_t input_count() const { return diag_input_bars_processed_; }
};
#endif

#if !defined(PF_GRID_NATIVE_MODULE) && !defined(PF_GRID_PINE_MODULE)
int drive(const std::string& mode, const char* timezone, const char* session,
          const char* width, int warmup_count) {
    GridNative host;
    auto refused = [&](const char* step) {
        std::cerr << "GRID ERROR " << step << ' ' << host.last_run_status()
                  << ' ' << host.last_error() << '\n';
        return 2;
    };
    NativeRunSpec spec;
    spec.identity = {"native-chart-grid", 1};
    spec.input_tf = "1";
    spec.script_tf = width;
    spec.tickerid = "TEST:GRID";
    spec.timezone = timezone;
    spec.session = session;
    spec.initial_capital = 100000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied)
        return refused("configure");

    std::vector<Bar> bars;
    Bar b{};
    while (std::cin >> b.timestamp >> b.open >> b.high >> b.low >> b.close >> b.volume)
        bars.push_back(b);
    if (!std::cin.eof() || bars.empty() || warmup_count < 0 ||
        static_cast<std::size_t>(warmup_count) >= bars.size())
        return refused("input-fixture");

    if (mode == "batch") {
        if (warmup_count != 0) return refused("batch-warmup");
        host.run(bars.data(), static_cast<int>(bars.size()));
    } else {
        if (!host.stream_begin(bars.data(), warmup_count, spec.input_tf, spec.script_tf))
            return refused("stream-begin");
        std::uint64_t seq = 0;
        for (std::size_t i = static_cast<std::size_t>(warmup_count); i < bars.size(); ++i) {
            const auto& minute = bars[i];
            if (mode == "bars") {
                if (!host.stream_push_bar(minute)) return refused("push-bar");
            } else {
                const double prices[] = {minute.open, minute.high, minute.low, minute.close};
                for (int j = 0; j < 4; ++j) {
                    if (!host.stream_push_tick({minute.timestamp + j * 1000,
                                                ++seq, prices[j], minute.volume / 4}))
                        return refused("push-tick");
                }
                if (!host.stream_advance_time(minute.timestamp + 60000))
                    return refused("advance-clock");
            }
        }
        // Reliance on stream_end to flush a forming bar would differ from
        // the runner. Snapshot callbacks and input count before ending.
        std::cerr << "GRID BEFORE END\n";
        if (!host.stream_end(false)) return refused("stream-end");
    }
    if (host.last_run_status() != 0 || host.native_state().kind != NativeLifecycleKind::Completed)
        return refused("completed-state");
    const auto end = bars.back().timestamp;
    std::cout << "GRID INPUT " << mode << ' ' << timezone << ' ' << session << ' ' << width
              << ' ' << warmup_count << ' ' << host.input_count()
              << ' ' << bars.front().timestamp << ' ' << end << '\n';
    return 0;
}
#endif
}  // namespace

#if defined(PF_GRID_NATIVE_MODULE)
PINEFORGE_EXPORT_NATIVE_STRATEGY(GridNative);
#elif defined(PF_GRID_PINE_MODULE)
extern "C" {
PF_API void* strategy_create(const char*) {
    try { return new GridPine; }
    catch (...) { return nullptr; }
}
PF_API void strategy_free(void* s) { delete static_cast<GridPine*>(s); }
PF_API void strategy_set_input(void* s, const char* key, const char* value) {
    static_cast<GridPine*>(s)->set_input(key, value);
}
// The observer declares no settings overrides, like the runner example.
PF_API void strategy_set_override(void*, const char*, const char*) {}
PF_API void report_free(pf_report_t* out) { pineforge::native_module::report_free(out); }
PF_API int grid_probe_link_runtime() { return pf_abi_version(); }
}
#else
int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "GRID ERROR expected mode timezone session chart-width warmup-count\n";
        return 2;
    }
    const std::string mode(argv[1]);
    if (mode != "batch" && mode != "bars" && mode != "ticks") {
        std::cerr << "GRID ERROR unknown engine input mode\n";
        return 2;
    }
    return drive(mode, argv[2], argv[3], argv[4], std::stoi(argv[5]));
}
#endif
