// The run's matrix Runtime is bound to the run, on one thread.
//
// The script (a5m_run_script.hpp) installs no matrix Runtime; the engine makes
// the run's own current for every public call that runs script code
// (src/engine_consumer.cpp). Rows:
//  - consecutive runs of one strategy: the second run opens a fresh Runtime,
//    and by its first bar the first run's Runtime (and its scratch bytes) is
//    closed; no Runtime is current on the thread between runs.
//  - two strategies on one thread: each run has its own Runtime and Store,
//    holding only its own matrices; destroying a strategy closes its Runtime.
//  - a stream keeps one Runtime from stream_begin through every push; after
//    stream_end the next run on the strategy opens a fresh one.
//  - outside any run a matrix is native: the heap path stays for library use.
//  - a caller's explicit Runtime still decides inside a run (the A5M rows that
//    install their own keep their own Store); the run then opens none.
//
// FAIL-CLOSED. Without the engine's install the first execution of the first
// run finds no current Runtime ("no matrix Runtime is current inside the run").
// A Runtime kept past its run, or one shared by runs or by threads' runs,
// fails "the second run reused the first run's Runtime" or "two strategies on
// one thread share a matrix Runtime".
#include "a5m_run_script.hpp"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

using a5m_run::Expect;
using a5m_run::Options;
using a5m_run::Script;
using a5m_test::require;
namespace ms = pineforge::detail::matrix_store;

constexpr int kBars = 32;

void run_batch(Script& script, const std::vector<pineforge::Bar>& bars) {
    script.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    require(script.last_error().empty(), script.last_error().c_str());
    require(ms::current_runtime() == nullptr,
            "a matrix Runtime is current on the thread after the run");
    a5m_run::require_or_exit(!script.runtime.expired(),
                             "the run's Runtime closed while its matrices were alive");
    script.verify_placement();
    script.verify_final(static_cast<int>(bars.size()));
}

// The first bar of a lifecycle found a fresh Runtime holding only the
// script's own matrices, and the previous lifecycle's Runtime closed.
void require_fresh(const Script& script) {
    require(!script.previous_same_at_first_bar, "the second run reused the first run's Runtime");
    require(script.previous_closed_at_first_bar,
            "the first run's matrix Runtime was still open in the second run");
    require(script.objects_at_first_bar == a5m_run::kScriptMatrices,
            "a run's Store holds matrices the run did not create");
}

void consecutive_runs() {
    const auto bars = a5m_run::tape(kBars);
    Script script(Options{});
    run_batch(script, bars);
    const std::weak_ptr<ms::Runtime> first = script.runtime;
    require(!first.expired(), "the run's Runtime closed while its matrices were alive");
    require(script.objects_at_first_bar == a5m_run::kScriptMatrices,
            "a run's Store holds matrices the run did not create");
    run_batch(script, bars);
    require(script.lifecycles == 2, "the second run did not start a new script lifecycle");
    require_fresh(script);
    require(first.expired(), "the first run's matrix Runtime outlived the second run");
    std::printf("A5M run lifetime: consecutive runs open %s Runtimes\n",
                script.previous_same_at_first_bar ? "one" : "two");
}

void two_strategies() {
    const auto bars = a5m_run::tape(kBars);
    std::weak_ptr<ms::Runtime> a_runtime, b_runtime;
    {
        Script a(Options{});
        run_batch(a, bars);
        a_runtime = a.runtime;
        {
            Script b(Options{});
            run_batch(b, bars);
            b_runtime = b.runtime;
            require(b.objects_at_first_bar == a5m_run::kScriptMatrices,
                    "a run's Store holds another strategy's matrices");
            require(!a_runtime.expired() && !b_runtime.expired() &&
                        a_runtime.lock() != b_runtime.lock(),
                    "two strategies on one thread share a matrix Runtime");
            require(a_runtime.lock()->store().stats().objects_live == a5m_run::kScriptMatrices &&
                        b_runtime.lock()->store().stats().objects_live ==
                            a5m_run::kScriptMatrices,
                    "a run's Store holds another strategy's matrices");
        }
        require(b_runtime.expired(), "destroying the strategy left its matrix Runtime open");
        require(!a_runtime.expired(), "another strategy's destruction closed this one's Runtime");
    }
    require(a_runtime.expired(), "destroying the strategy left its matrix Runtime open");
}

void stream_then_batch() {
    constexpr int kWarmup = 8;
    const auto bars = a5m_run::tape(kBars);
    Script script(Options{});
    require(script.stream_begin(bars.data(), kWarmup, "1", "1"), script.last_error().c_str());
    require(ms::current_runtime() == nullptr,
            "a matrix Runtime is current on the thread after stream_begin");
    const std::weak_ptr<ms::Runtime> warmup = script.runtime;
    a5m_run::require_or_exit(!warmup.expired(),
                             "the stream's Runtime closed while its matrices were alive");
    for (int i = kWarmup; i < kBars; ++i) {
        // Each push's execution requires the Runtime it found at the first bar.
        require(script.stream_push_bar(bars[static_cast<std::size_t>(i)]),
                script.last_error().c_str());
        require(ms::current_runtime() == nullptr,
                "a matrix Runtime is current on the thread between stream pushes");
    }
    require(script.lifecycles == 1 && script.executions == kBars && script.rows == kBars,
            "the stream did not execute every bar in one lifecycle");
    require(script.runtime.lock() == warmup.lock(), "a stream push opened a second Runtime");
    script.verify_final(kBars);
    (void)script.stream_end(false);
    a5m_run::require_or_exit(!warmup.expired(),
                             "stream_end closed a Runtime its matrices still read");
    run_batch(script, bars);
    require_fresh(script);
}

void outside_any_run() {
    require(ms::current_runtime() == nullptr, "a matrix Runtime is current outside any run");
    const auto numeric = pineforge::PineMatrix::new_(2, 2, 1.0);
    const auto integer = pineforge::PineGenericMatrix<int>::new_(2, 2, 7);
    const auto flags = pineforge::PineGenericMatrix<bool>::new_(2, 2, true);
    require(!numeric.is_external_storage() && !integer.is_external_storage() &&
                !flags.is_external_storage(),
            "a matrix made outside any run left the heap");
    require(numeric.get(1, 1) == 1.0 && integer.get(1, 0) == 7 && flags.get(0, 1),
            "a native matrix read a wrong cell");
}

void explicit_runtime() {
    const auto bars = a5m_run::tape(kBars);
    a5m_test::StoreFixture fixture;
    ms::Runtime chosen(*fixture.store);
    const ms::ScopedRuntime scope(chosen);
    Options options;
    options.expect = Expect::explicit_runtime;
    options.chosen = &chosen;
    {
        Script script(options);
        script.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
        require(script.last_error().empty(), script.last_error().c_str());
        script.verify_placement();
        script.verify_final(static_cast<int>(bars.size()));
        require(ms::current_runtime() == &chosen, "the run did not hand the caller's choice back");
        require(fixture.store->stats().objects_live == a5m_run::kScriptMatrices,
                "the script's matrices are not in the caller's Store");
    }
    require(fixture.store->stats().objects_live == 0,
            "the strategy's matrices outlived it in the caller's Store");
}

} // namespace

int main() {
    bool ok = true;
    const auto row = [&ok](const char* name, void (*body)()) {
        try {
            body();
        } catch (const std::exception& error) {
            ok = false;
            std::fprintf(stderr, "A5M run lifetime %s: %s\n", name, error.what());
        }
    };
    row("outside any run", outside_any_run);
    row("consecutive runs", consecutive_runs);
    row("two strategies", two_strategies);
    row("stream then batch", stream_then_batch);
    row("explicit runtime", explicit_runtime);
    return ok ? 0 : 1;
}
