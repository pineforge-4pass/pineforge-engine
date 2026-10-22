// CR150-CX open-lot scaling probe (comparison between v1.6.1 and the join head).
//
// Measures ONLY the engine's per-bar cost with N open lots: one host holds a
// native strategy that submits one market lot per bar and never closes, runs N+4
// bars, then builds the full report. It prints, for each phase, wall and process
// CPU time:
//   R12CMP lots=<N> phase=<run|report> wall_ms=<..> cpu_ms=<..>
// and passes (exit 0) if the population really reached N open lots and the report
// carries N rows. It has no timing bound: the numbers are read by a person.
// The source is identical in both trees; only the registration differs.
#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <vector>

#ifndef OPEN_LOTS_N
#error "define OPEN_LOTS_N"
#endif

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

constexpr std::int64_t kT0 = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;

NativeRunSpec hold_spec() {
    NativeRunSpec spec;
    spec.identity = {"cmp-open-lots", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    return spec;
}

std::vector<Bar> rising_tape(int bars) {
    std::vector<Bar> tape;
    tape.reserve(static_cast<std::size_t>(bars));
    for (int i = 0; i < bars; ++i) {
        const double base = 100.0 + static_cast<double>(i) * 0.015625;
        tape.push_back(Bar{base, base + 0.5, base - 0.5, base + 0.25, 1000.0,
                           kT0 + i * kMinute});
    }
    return tape;
}

class HoldHost final : public NativeStrategyHost {
public:
    explicit HoldHost(int target) : target_(target) {}
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (submitted_ >= target_) return;
        const auto result = submit({no::Transact{1.0}, "lot", ""});
        if (result.status != no::SubmitStatus::Accepted || !result.handle.has_value()) {
            rejected_ = true;
            return;
        }
        ++submitted_;
    }
    std::size_t open_lots() const { return pyramid_entries_.size(); }
    bool rejected() const { return rejected_; }

private:
    int target_;
    int submitted_ = 0;
    bool rejected_ = false;
};

double wall_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - since).count();
}
double cpu_ms(std::clock_t since) {
    return 1000.0 * static_cast<double>(std::clock() - since) / CLOCKS_PER_SEC;
}

}  // namespace

int main() {
    constexpr int lots = OPEN_LOTS_N;
    HoldHost host(lots);
    const auto setup = host.configure_native(hold_spec());
    if (setup.status != NativeSetupStatus::Applied) {
        std::fprintf(stderr, "FAIL configure_native\n");
        return 1;
    }
    const auto bars = rising_tape(lots + 4);

    auto w0 = std::chrono::steady_clock::now();
    auto c0 = std::clock();
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::fprintf(stderr, "R12CMP lots=%d phase=run wall_ms=%.1f cpu_ms=%.1f\n",
                 lots, wall_ms(w0), cpu_ms(c0));
    if (!host.last_error().empty() || host.rejected()
        || host.open_lots() != static_cast<std::size_t>(lots)) {
        std::fprintf(stderr, "FAIL population: open_lots=%zu rejected=%d error=%s\n",
                     host.open_lots(), host.rejected() ? 1 : 0, host.last_error().c_str());
        return 1;
    }

    w0 = std::chrono::steady_clock::now();
    c0 = std::clock();
    ReportC report{};
    host.fill_report(&report);
    std::fprintf(stderr, "R12CMP lots=%d phase=report wall_ms=%.1f cpu_ms=%.1f\n",
                 lots, wall_ms(w0), cpu_ms(c0));
    const bool ok = report.total_trades == lots && report.trades_len == lots;
    BacktestEngine::free_report(&report);
    if (!ok) {
        std::fprintf(stderr, "FAIL report rows\n");
        return 1;
    }
    return 0;
}
