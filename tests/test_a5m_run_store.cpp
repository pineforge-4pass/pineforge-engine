// Matrices in the run's own Store inside real strategy runs.
//
// The script (a5m_run_script.hpp) is laid out as generated code and installs
// no matrix Runtime: the engine makes the run's own current for every public
// call that runs script code (src/engine_consumer.cpp). Three rows:
//  - growth: 8192 bars each append a row to three growing matrices. Every
//    matrix is in the run's own Store, the Store holds the population, and the
//    heap the matrix work retains after the warmup stays under the fixed bound
//    the A5M growth rows use, with the run's fixed resident tier counted in it.
//    The same script under an explicit native choice
//    is the control: its heap crosses that bound (the measurement can see
//    growth), and its cells and trades are the same bytes.
//  - calc-on-fill: a calc_on_order_fills run's checkpoints and restores (the
//    generated Snapshot/restore) over Store matrices leave one exact row per
//    bar, and the Store holds exactly the matrices and their checkpoint.
//  - the call's transaction: a stream push whose execution fails inside a
//    matrix operation fails, and its matrix changes roll back with the call;
//    the rows the warmup and the earlier pushes committed remain, exact.
//
// FAIL-CLOSED. Without the engine's install the first execution finds no
// current Runtime ("no matrix Runtime is current inside the run"). With an
// engine that keeps no share of its Runtime past the run but Cells that do
// not share it either, the Runtime dies with the run ("the run's Runtime closed
// while its matrices were alive"). Without the call's transaction the failing
// push succeeds ("the push whose matrix operation failed did not fail").
#include "a5m_run_script.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using a5m_run::Expect;
using a5m_run::Options;
using a5m_run::Script;
using a5m_test::require;
namespace ms = pineforge::detail::matrix_store;

constexpr int kGrowthBars = 8192;
static_assert(a5m_test::kTierBound == ms::kResidentBytes,
              "the growth bound counts the run's resident tier at its fixed size");

void growth() {
    const auto bars = a5m_run::tape(kGrowthBars);
    // The run's Store, one operation's buffers and the run's resident tier.
    const auto bound = a5m_test::kStoreBound + a5m_test::kOperationBound + a5m_test::kTierBound;
    require(static_cast<std::uint64_t>(kGrowthBars) * a5m_run::kRowBytes > bound,
            "growth population cannot exceed the claimed bound");
    Options options;
    options.trade_every = 64;
    options.heap_windows = true;
    std::string store_trades;
    std::weak_ptr<ms::Runtime> closed;
    global_allocation::live_bytes = 0;
    {
        Script script(options);
        require(ms::current_runtime() == nullptr, "a matrix Runtime is current before any run");
        script.run(bars.data(), kGrowthBars, "1", "1", false);
        require(script.last_error().empty(), script.last_error().c_str());
        require(script.executions == kGrowthBars && script.lifecycles == 1,
                "the run did not execute every bar once");
        require(ms::current_runtime() == nullptr,
                "a matrix Runtime is current on the thread after the run");
        const std::shared_ptr<ms::Runtime> runtime = script.runtime.lock();
        a5m_run::require_or_exit(runtime != nullptr,
                                 "the run's Runtime closed while its matrices were alive");
        script.verify_placement();
        const auto stats = runtime->store().stats();
        std::printf("A5M run growth bars=%d retained=%lld requested=%zu bound=%llu live=%llu "
                    "objects=%llu backing=%llu %s\n",
                    kGrowthBars, static_cast<long long>(script.peak), script.requested_peak,
                    static_cast<unsigned long long>(bound),
                    static_cast<unsigned long long>(stats.bytes_live),
                    static_cast<unsigned long long>(stats.objects_live),
                    static_cast<unsigned long long>(stats.bytes_backing),
                    script.fingerprint().c_str());
        require(stats.objects_live == a5m_run::kScriptMatrices,
                "the run's Store does not hold exactly the script's matrices");
        require(stats.bytes_live >= static_cast<std::uint64_t>(kGrowthBars) * a5m_run::kRowBytes,
                "the run's Store does not hold the matrix population");
        require(stats.cleanup_failed == 0 && stats.cleanup_skipped_rollback_only == 0,
                "a call's matrix transaction was refused");
        require(script.peak <= static_cast<std::int64_t>(bound) && script.requested_peak <= bound,
                "matrix retained heap grows with the cell population");
        script.verify_final(kGrowthBars);
        require(script.trade_count() > 0, "the matrix-driven orders never traded");
        store_trades = script.fingerprint();
        closed = script.runtime;
    }
    require(closed.expired(), "destroying the strategy left its matrix Runtime open");

    // The control: the same script under an explicit native choice.
    global_allocation::live_bytes = 0;
    {
        Options native = options;
        native.expect = Expect::explicit_native;
        const ms::ScopedRuntime choice(nullptr);
        Script script(native);
        script.run(bars.data(), kGrowthBars, "1", "1", false);
        require(script.last_error().empty(), script.last_error().c_str());
        script.verify_placement();
        script.verify_final(kGrowthBars);
        std::printf("A5M run growth native control retained=%lld requested=%zu %s\n",
                    static_cast<long long>(script.peak), script.requested_peak,
                    script.fingerprint().c_str());
        // The allocation counter, not the allocator's statistics: it counts
        // under a sanitizer's allocator as well.
        require(script.requested_peak > bound,
                "the native control's heap never crossed the bound: "
                "the heap measurement cannot see growth");
        require(script.fingerprint() == store_trades,
                "the Store run's trades differ from the native run's");
    }
}

void calc_on_fill() {
    constexpr int kBars = 96;
    const auto bars = a5m_run::tape(kBars);
    Options options;
    options.coof = true;
    options.trade_every = 8;
    Script script(options);
    script.run(bars.data(), kBars, "1", "1", false);
    require(script.last_error().empty(), script.last_error().c_str());
    require(script.restores > 0 && script.executions > kBars && script.trade_count() > 0,
            "real calc_on_order_fills was not reached");
    const std::shared_ptr<ms::Runtime> runtime = script.runtime.lock();
    a5m_run::require_or_exit(runtime != nullptr,
                             "the run's Runtime closed while its matrices were alive");
    script.verify_placement();
    script.verify_final(kBars);
    const auto stats = runtime->store().stats();
    std::printf("A5M run calc-on-fill snapshots=%d restores=%d executions=%d objects=%llu %s\n",
                script.snapshots, script.restores, script.executions,
                static_cast<unsigned long long>(stats.objects_live), script.fingerprint().c_str());
    require(stats.cleanup_failed == 0 && stats.cleanup_skipped_rollback_only == 0,
            "a call's matrix transaction was refused");
    // The four matrices and the four snapshots of the last checkpoint.
    require(stats.objects_live == 2 * a5m_run::kScriptMatrices,
            "the run's Store does not hold exactly the matrices and their checkpoint");
}

void failed_call() {
    constexpr int kWarmup = 8, kBars = 24;
    const auto bars = a5m_run::tape(kBars);
    Script script(Options{});
    require(script.stream_begin(bars.data(), kWarmup, "1", "1"), script.last_error().c_str());
    for (int i = kWarmup; i < kBars - 4; ++i)
        require(script.stream_push_bar(bars[static_cast<std::size_t>(i)]),
                script.last_error().c_str());
    const int committed = script.rows;
    require(committed > kWarmup, "the stream pushes executed no bar");
    const std::shared_ptr<ms::Runtime> runtime = script.runtime.lock();
    a5m_run::require_or_exit(runtime != nullptr,
                             "the stream's Runtime closed while its matrices were alive");
    script.verify_cells(committed);
    const int executions = script.executions;
    script.poison_next = true;
    require(!script.stream_push_bar(bars[static_cast<std::size_t>(kBars - 4)]),
            "the push whose matrix operation failed did not fail");
    require(script.executions == executions + 1 && !script.poison_next,
            "the failing push did not execute its bar");
    require(!script.last_error().empty(), "the failed push recorded no error");
    // The call's matrix changes rolled back with it; earlier calls' rows stay.
    script.verify_cells(committed);
    const auto stats = runtime->store().stats();
    std::printf("A5M run failed call committed=%d objects=%llu rollback_only=%llu error='%s'\n",
                committed, static_cast<unsigned long long>(stats.objects_live),
                static_cast<unsigned long long>(stats.cleanup_skipped_rollback_only),
                script.last_error().c_str());
    require(stats.cleanup_skipped_rollback_only == 1 && stats.cleanup_failed == 0,
            "the failed call's transaction was not refused and rolled back");
    require(stats.objects_live == a5m_run::kScriptMatrices,
            "the failed call left a matrix object behind");
    (void)script.stream_end(false);
}

} // namespace

int main() {
    bool ok = true;
    const auto row = [&ok](const char* name, void (*body)()) {
        try {
            body();
        } catch (const std::exception& error) {
            global_allocation::recording = false;
            ok = false;
            std::fprintf(stderr, "A5M run %s: %s\n", name, error.what());
        }
    };
    row("growth", growth);
    row("calc-on-fill", calc_on_fill);
    row("failed call", failed_call);
    return ok ? 0 : 1;
}
