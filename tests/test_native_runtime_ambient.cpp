// R5 lane D2-C: the library's per-thread runtime state has one home per pump,
// and every value it answers is the value thread-local storage answered.
//
// The TA bar context (ta::bar_context), the EMA seeding default
// (ta::ema_na_warmup_flag) and the chart day partition
// (active_native_day_partition) are the library's per-thread runtime state
// (src/runtime_ambient.hpp). A host that sets them around every calculation --
// the Pine host around every script bar it publishes, the request.security
// evaluators around every evaluation -- now writes the consumer's runtime
// block, the thread's block in force for the length of a pump
// (NativeExecutionConsumer::pump_ambient), instead of thread-local storage;
// every reader still finds the state through the thread. This TU holds the
// mechanism to its reference on bare hosts; test_adapter_runtime_ambient.cpp
// holds the Pine host's publications to theirs.
//
//   1. Hosts over K3's randomized books whose every callback -- input, bar
//      open, walked lower-timeframe sub-bar, calculation, fill recalculation,
//      applied fill -- opens the three scopes a Pine publication opens
//      (internal::Ambient*Scope on the pump's block), reads them back through
//      TA members (bar-addressed extremum rings every bar and on a cadence,
//      EMAs latching their seeding at different callbacks, ta.vwap's session
//      anchor) and the calendar (session_day_index and timeframe.change("1D")
//      under a day partition that merges two days), and records the state
//      outside its scopes. With the block (the shipped path) and without it
//      (set_runtime_ambient(false): thread-local storage at every write, the
//      path before the lane) every value read, every run outcome and the
//      thread's state after the run agree. Lower-timeframe runs are labeled
//      Canonical so their sub-bars are walked (D2-A finding 3); the
//      synthesized path and calc_on_order_fills run too.
//   2. The block is taken: a block run installs it once per batch pump; a
//      reference run, and a host that never asks for it, never do.
//   3. The window: values set on the thread before a run are what the run's
//      readers see outside the host's scopes and what the thread holds after
//      it, and a value a callback leaves raised outside every scope stays
//      raised after the run, as it did without the block.
//   4. Many handles on one thread (two streams interleaved input by input, a
//      run nested in another host's calculation) and one handle per thread
//      (two threads at once): each host reads its solo values.
//
// Fail-before: at the lane's base the consumer has no pump_ambient and
// src/runtime_ambient.hpp does not exist, so this TU does not compile there
// (the lane report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "native_run_outcome_compare.hpp"

#include "../src/native_execution_consumer.hpp"
#include "../src/runtime_ambient.hpp"

#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace pineforge;

namespace {
using k3_book::BookConfig;
using k3_book::Path;
using outcome_compare::Tally;

Tally tally;
#define CHECK(condition) OUTCOME_CHECK(tally, condition)

constexpr std::int64_t kDay = 86400000LL;
constexpr std::int64_t kT0 = 1736121600000LL;  // the K3 tape's first bar, a UTC midnight

// The thread's runtime state as a reader sees it.
struct ThreadState {
    ta::BarContext context{};
    bool ema_na_warmup = false;
    const NativeDayPartition* partition = nullptr;
};

ThreadState thread_state() {
    return {ta::bar_context(), ta::ema_na_warmup_flag(), active_native_day_partition()};
}

bool same_state(const ThreadState& a, const ThreadState& b) {
    return a.context.installed == b.context.installed && a.context.bar_index == b.context.bar_index
        && a.context.origin == b.context.origin && a.ema_na_warmup == b.ema_na_warmup
        && a.partition == b.partition;
}

void set_thread_state(const ThreadState& state) {
    ta::bar_context() = state.context;
    ta::ema_na_warmup_flag() = state.ema_na_warmup;
    set_active_native_day_partition(state.partition);
}

// The calling thread has no block installed: every run handed its block back.
bool thread_owns_its_state() { return internal::tl_runtime_ambient.installed == nullptr; }

// A UTC day partition over `bars` whose first period merges the tape's first
// two days: session_day_index, timeframe.change("1D") and ta.vwap's anchor
// read it differently from the nominal calendar on the first day.
NativeDayPartition merged_day_partition(const std::vector<Bar>& bars) {
    NativeDayPartition partition;
    std::vector<std::int64_t> stamps;
    for (std::int64_t day = 0; day < 8; ++day) {
        if (day == 1) continue;
        stamps.push_back(kT0 + day * kDay);
    }
    const bool built = build_native_day_partition(partition, "UTC", "24x7", stamps,
                                                  bars.data(), static_cast<int>(bars.size()));
    CHECK(built);
    return partition;
}

// A partition of another clock: the chart's readers ignore it, and a reader
// of active_native_day_partition() sees it.
const NativeDayPartition& foreign_partition() {
    static const NativeDayPartition partition = [] {
        NativeDayPartition out;
        const std::vector<std::int64_t> stamps{kT0 - 2 * kDay, kT0 - kDay};
        const Bar bar{1.0, 1.0, 1.0, 1.0, 1.0, kT0 - 2 * kDay + 60000};
        build_native_day_partition(out, "America/New_York", "0930-1600", stamps, &bar, 1);
        return out;
    }();
    return partition;
}

// A K3 book whose every callback runs under the three scopes a Pine
// publication opens and reads them back.
class AmbientHost : public k3_book::BookHost {
public:
    AmbientHost(const BookConfig& config, const NativeDayPartition* partition)
        : BookHost(config), partition_(partition) {}

    // Every value read inside the scopes (TA members, calendar), and every
    // state read outside them, folded per callback.
    std::vector<std::uint64_t> inside;
    std::vector<std::uint64_t> outside;
    // Raise the EMA seeding default outside every scope at this applied fill
    // (-1: never); the write is left in place.
    int raise_at_applied = -1;
    // Run `nested` from the calculation of these bars.
    std::vector<int> nest_at;
    std::function<void()> nested;

    NativeExecutionConsumer& consumer() { return NativeExecutionConsumer::bound(*this); }

    void on_native_input(const Bar&, const NativeInputContext&) override { note_outside(); }

    void on_native_bar_open(const Bar& bar, const NativeDecisionContext& context) override {
        under_scopes(bar, context, 1, [] {});
    }

    void on_native_sub_bar(const Bar& sub, const NativeDecisionContext& context) override {
        under_scopes(sub, context, 2, [] {});
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        under_scopes(bar, context, 3, [&] {
            BookHost::on_native_bar(bar, context);
            const int calculations = ++calculations_;
            for (const int at : nest_at) {
                if (at == calculations && nested) nested();
            }
        });
    }

    void on_native_applied(const native_order::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        note_outside();
        BookHost::on_native_applied(event, context);
        if (raise_at_applied >= 0 && applied_++ == raise_at_applied)
            ta::ema_na_warmup_flag() = !ta::ema_na_warmup_flag();
    }

private:
    template <class Body>
    void under_scopes(const Bar& bar, const NativeDecisionContext& context, int kind, Body&& body) {
        internal::RuntimeAmbient* const block = consumer().pump_ambient();
        internal::AmbientDayPartitionScope day(block, partition_);
        // Bar opens raise the seeding default, the other callbacks lower it:
        // an EMA latches whichever callback first computes it.
        internal::AmbientEmaSeedingScope seeding(block, kind == 1);
        const long long index = context.coordinate.interval_index;
        internal::AmbientBarContextScope bar_context(block, index * 4 + kind, 3);
        std::uint64_t h = fold_state(1469598103934665603ull, thread_state());
        const std::int64_t ts = bar.timestamp;
        h = k3_book::fnv_f64(h, hi_.compute(bar.high));
        if (index % 3 == kind % 3) h = k3_book::fnv_f64(h, lo_.compute(bar.low));
        h = k3_book::fnv_f64(h, highest_bars_.compute(bar.close));
        // The first EMA latches at the first bar open (raised), the second at
        // a calculation (lowered).
        h = k3_book::fnv_f64(h, ema_every_.compute(bar.close));
        if (index >= 7 && kind == 3) h = k3_book::fnv_f64(h, ema_late_.compute(bar.close));
        const double hlc3 = (bar.high + bar.low + bar.close) / 3.0;
        h = k3_book::fnv_f64(h, vwap_.compute(hlc3, bar.volume, ts, "UTC", "24x7"));
        h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(session_day_index(ts, "UTC", "24x7")));
        h = k3_book::fnv_u64(h, tf_change(previous_ts_, ts, "1D", "UTC", "24x7") ? 1u : 0u);
        previous_ts_ = ts;
        inside.push_back(h);
        body();
    }

    void note_outside() { outside.push_back(fold_state(1469598103934665603ull, thread_state())); }

    static std::uint64_t fold_state(std::uint64_t h, const ThreadState& state) {
        h = k3_book::fnv_u64(h, state.context.installed ? 1u : 0u);
        h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(state.context.bar_index));
        h = k3_book::fnv_u64(h, static_cast<std::uint64_t>(state.context.origin));
        h = k3_book::fnv_u64(h, state.ema_na_warmup ? 1u : 0u);
        return k3_book::fnv_u64(h, reinterpret_cast<std::uintptr_t>(state.partition));
    }

    const NativeDayPartition* partition_;
    ta::Highest hi_{5};
    ta::Lowest lo_{4};
    ta::HighestBars highest_bars_{6};
    ta::EMA ema_every_{5};
    ta::EMA ema_late_{5};
    ta::VWAP vwap_;
    std::int64_t previous_ts_ = 0;
    int calculations_ = 0;
    int applied_ = 0;
};

struct Run {
    k3_book::Outcome outcome;
    std::vector<std::uint64_t> inside;
    std::vector<std::uint64_t> outside;
    std::uint64_t installs = 0;
    ThreadState after{};
    bool thread_owns = false;
};

// Runs `config` on a fresh host, with the block or without it.
template <class Prepare>
Run run_book(const BookConfig& config, const NativeDayPartition* partition, bool block,
             Prepare&& prepare) {
    AmbientHost host(config, partition);
    host.consumer().set_runtime_ambient(block);
    prepare(host);
    Run run;
    run.outcome = outcome_compare::run_book(host, config, outcome_compare::as_is);
    run.inside = host.inside;
    run.outside = host.outside;
    run.installs = host.consumer().runtime_ambient_installs();
    run.after = thread_state();
    run.thread_owns = thread_owns_its_state();
    return run;
}

Run run_book(const BookConfig& config, const NativeDayPartition* partition, bool block) {
    return run_book(config, partition, block, [](AmbientHost&) {});
}

bool same_run(const Run& a, const Run& b) {
    const int before = tally.failures;
    outcome_compare::same(tally, a.outcome, b.outcome);
    outcome_compare::first_divergence(tally, a.inside, b.inside, "inside-scope read");
    outcome_compare::first_divergence(tally, a.outside, b.outside, "outside-scope read");
    CHECK(same_state(a.after, b.after));
    return tally.failures == before;
}

std::vector<BookConfig> configs(std::uint64_t first_seed) {
    std::vector<BookConfig> out;
    std::uint64_t seed = first_seed;
    for (const int live : {1, 6, 25}) {
        for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
            for (int variant = 0; variant < 2; ++variant) {
                BookConfig config;
                config.seed = seed++;
                config.live = live;
                // Three days of five-minute bars for the day partition.
                config.bars = live >= 25 ? 120 : 864;
                config.path = path;
                config.calc_on_fills = variant == 1;
                out.push_back(config);
            }
        }
    }
    return out;
}

long total_reads = 0;
long total_sub_bars = 0;

// 1 + 2: every value read agrees with the reference, and the block is taken.
void block_matches_thread_local_storage() {
    int runs = 0;
    for (const auto& config : configs(91001)) {
        const k3_book::Tape tape = k3_book::make_tape(config);
        const NativeDayPartition partition = merged_day_partition(tape.bars);
        const ThreadState before = thread_state();
        const Run block = run_book(config, &partition, true);
        CHECK(block.thread_owns);
        CHECK(same_state(block.after, before));
        const Run reference = run_book(config, &partition, false);
        CHECK(reference.thread_owns);
        if (!same_run(block, reference)) outcome_compare::describe(config, "");
        CHECK(block.outcome.completed);
        CHECK(!block.inside.empty());
        CHECK(block.installs == 1);
        CHECK(reference.installs == 0);
        total_reads += static_cast<long>(block.inside.size() + block.outside.size());
        if (config.path == Path::Lower) {
            // Walked sub-bars: more scoped callbacks than bar opens and
            // calculations alone.
            const auto opens_and_bars = static_cast<std::size_t>(2 * config.bars);
            CHECK(block.inside.size() > opens_and_bars);
            total_sub_bars += static_cast<long>(block.inside.size() - opens_and_bars);
        }
        ++runs;
    }
    std::printf("books: %d configurations, %ld reads, %ld walked sub-bar reads beyond opens "
                "and calculations\n", runs, total_reads, total_sub_bars);
    CHECK(total_sub_bars > 0);
}

// 2: a host that never asks for the block never installs it.
void a_host_that_never_asks_installs_nothing() {
    BookConfig config;
    config.seed = 92001;
    config.bars = 90;
    config.path = Path::Lower;
    k3_book::BookHost host(config);
    outcome_compare::run_book(host, config, outcome_compare::as_is);
    CHECK(host.outcome.completed);
    CHECK(NativeExecutionConsumer::bound(host).runtime_ambient_installs() == 0);
    CHECK(thread_owns_its_state());
}

// 3: the window onto the thread's own state.
void the_block_is_a_window() {
    const ThreadState preset{{true, 4242, 7}, true, &foreign_partition()};
    for (const Path path : {Path::None, Path::Lower}) {
        BookConfig config;
        config.seed = 93001 + static_cast<std::uint64_t>(path);
        config.live = 6;
        config.bars = 300;
        config.path = path;
        const k3_book::Tape tape = k3_book::make_tape(config);
        const NativeDayPartition partition = merged_day_partition(tape.bars);
        set_thread_state(preset);
        const Run block = run_book(config, &partition, true);
        CHECK(same_state(block.after, preset));
        set_thread_state(preset);
        const Run reference = run_book(config, &partition, false);
        CHECK(same_state(reference.after, preset));
        if (!same_run(block, reference)) outcome_compare::describe(config, "preset");
        // The readers outside the scopes saw the thread's own values.
        std::uint64_t first_outside = 0;
        {
            AmbientHost probe(config, &partition);
            set_thread_state(preset);
            probe.consumer().set_runtime_ambient(true);
            outcome_compare::run_book(probe, config, outcome_compare::as_is);
            CHECK(!probe.outside.empty());
            if (!probe.outside.empty()) first_outside = probe.outside.front();
        }
        set_thread_state(ThreadState{});
        const Run plain = run_book(config, &partition, true);
        CHECK(!plain.outside.empty());
        if (!plain.outside.empty()) CHECK(plain.outside.front() != first_outside);

        // A value a callback leaves raised outside every scope stays raised.
        set_thread_state(ThreadState{});
        const auto raise = [](AmbientHost& host) { host.raise_at_applied = 3; };
        const Run raised_block = run_book(config, &partition, true, raise);
        CHECK(raised_block.outcome.applied > 3);
        CHECK(raised_block.after.ema_na_warmup);
        set_thread_state(ThreadState{});
        const Run raised_reference = run_book(config, &partition, false, raise);
        CHECK(raised_reference.after.ema_na_warmup);
        if (!same_run(raised_block, raised_reference)) outcome_compare::describe(config, "raised");
        CHECK(raised_block.thread_owns);
        set_thread_state(ThreadState{});
    }
    std::printf("window: preset thread state seen outside the scopes and kept after the run\n");
}

// 4a: two streams interleaved input by input on one thread.
struct StreamRun {
    std::vector<std::uint64_t> inside;
    std::vector<std::uint64_t> outside;
    std::uint64_t broker = 0;
    int trades = 0;
};

void begin_stream(AmbientHost& host, const BookConfig& config, const k3_book::Tape& tape,
                  const NativeDayPartition*, int warmup) {
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    CHECK(host.stream_begin(tape.bars.data(), warmup, "5", "5"));
}

StreamRun finish_stream(AmbientHost& host) {
    CHECK(host.stream_end());
    host.finish();
    return {host.inside, host.outside, host.outcome.broker, host.outcome.trades};
}

StreamRun solo_stream(const BookConfig& config, const NativeDayPartition* partition, bool block) {
    const k3_book::Tape tape = k3_book::make_tape(config);
    AmbientHost host(config, partition);
    host.consumer().set_runtime_ambient(block);
    const int warmup = config.bars / 3;
    begin_stream(host, config, tape, partition, warmup);
    for (int i = warmup; i < config.bars; ++i) {
        CHECK(host.stream_push_bar(tape.bars[static_cast<std::size_t>(i)]));
    }
    return finish_stream(host);
}

bool same_stream(const StreamRun& a, const StreamRun& b) {
    const int before = tally.failures;
    outcome_compare::first_divergence(tally, a.inside, b.inside, "stream inside-scope read");
    outcome_compare::first_divergence(tally, a.outside, b.outside, "stream outside-scope read");
    CHECK(a.broker == b.broker);
    CHECK(a.trades == b.trades);
    return tally.failures == before;
}

void interleaved_streams_read_their_solo_values() {
    BookConfig a_config;
    a_config.seed = 94001;
    a_config.live = 6;
    a_config.bars = 600;
    BookConfig b_config = a_config;
    b_config.seed = 94002;
    const k3_book::Tape a_tape = k3_book::make_tape(a_config);
    const k3_book::Tape b_tape = k3_book::make_tape(b_config);
    const NativeDayPartition a_partition = merged_day_partition(a_tape.bars);
    for (const bool block : {true, false}) {
        const StreamRun a_solo = solo_stream(a_config, &a_partition, block);
        const StreamRun b_solo = solo_stream(b_config, nullptr, block);
        AmbientHost a(a_config, &a_partition);
        AmbientHost b(b_config, nullptr);
        a.consumer().set_runtime_ambient(block);
        b.consumer().set_runtime_ambient(block);
        const int warmup = a_config.bars / 3;
        begin_stream(a, a_config, a_tape, &a_partition, warmup);
        begin_stream(b, b_config, b_tape, nullptr, warmup);
        for (int i = warmup; i < a_config.bars; ++i) {
            CHECK(a.stream_push_bar(a_tape.bars[static_cast<std::size_t>(i)]));
            CHECK(thread_owns_its_state());
            CHECK(b.stream_push_bar(b_tape.bars[static_cast<std::size_t>(i)]));
            CHECK(thread_owns_its_state());
        }
        const StreamRun a_mixed = finish_stream(a);
        const StreamRun b_mixed = finish_stream(b);
        CHECK(same_stream(a_mixed, a_solo));
        CHECK(same_stream(b_mixed, b_solo));
        if (block) {
            // One install per stream input that ran a scoped callback.
            CHECK(a.consumer().runtime_ambient_installs() > 0);
            CHECK(b.consumer().runtime_ambient_installs() > 0);
        }
    }
    std::printf("interleaved streams: each host reads its solo values\n");
}

// 4b: a run nested in another host's calculation. The nested host's scoped
// reads are its solo reads; what it reads outside its scopes is the outer
// host's scope state, block or not.
void a_nested_run_reads_its_own_scopes() {
    BookConfig outer_config;
    outer_config.seed = 95001;
    outer_config.live = 6;
    outer_config.bars = 300;
    outer_config.path = Path::Lower;
    BookConfig inner_config;
    inner_config.seed = 95002;
    inner_config.live = 1;
    inner_config.bars = 40;
    const k3_book::Tape outer_tape = k3_book::make_tape(outer_config);
    const NativeDayPartition partition = merged_day_partition(outer_tape.bars);
    const Run inner_solo = run_book(inner_config, nullptr, true);
    std::vector<Run> nested_runs[2];
    Run outers[2];
    for (const bool block : {true, false}) {
        auto& inners = nested_runs[block ? 0 : 1];
        outers[block ? 0 : 1] = run_book(outer_config, &partition, block, [&](AmbientHost& host) {
            host.nest_at = {5, 17, 40};
            host.nested = [&] {
                inners.push_back(run_book(inner_config, nullptr, block));
                // The inner pump handed the thread back to the outer's block.
                CHECK(!thread_owns_its_state() || !block);
            };
        });
        CHECK(outers[block ? 0 : 1].thread_owns);
        CHECK(inners.size() == 3);
        for (const Run& inner : inners) {
            CHECK(inner.outcome.completed);
            outcome_compare::first_divergence(tally, inner.inside, inner_solo.inside,
                                              "nested inside-scope read");
            CHECK(inner.outcome.broker == inner_solo.outcome.broker);
        }
    }
    same_run(outers[0], outers[1]);
    for (std::size_t i = 0; i < nested_runs[0].size() && i < nested_runs[1].size(); ++i) {
        outcome_compare::first_divergence(tally, nested_runs[0][i].outside,
                                          nested_runs[1][i].outside, "nested outside-scope read");
    }
    std::printf("nested runs: %zu inner runs read their own scopes\n", nested_runs[0].size());
}

// 4c: one handle per thread, two threads at once.
void two_threads_read_their_solo_values() {
    BookConfig c_config;
    c_config.seed = 96001;
    c_config.live = 6;
    c_config.bars = 600;
    c_config.path = Path::Synthesized;
    BookConfig d_config = c_config;
    d_config.seed = 96002;
    d_config.path = Path::Lower;
    const k3_book::Tape c_tape = k3_book::make_tape(c_config);
    const k3_book::Tape d_tape = k3_book::make_tape(d_config);
    const NativeDayPartition c_partition = merged_day_partition(c_tape.bars);
    const NativeDayPartition d_partition = merged_day_partition(d_tape.bars);
    const Run c_solo = run_book(c_config, &c_partition, true);
    const Run d_solo = run_book(d_config, &d_partition, true);
    Run c_threaded;
    Run d_threaded;
    for (int round = 0; round < 3; ++round) {
        std::thread other([&] { c_threaded = run_book(c_config, &c_partition, true); });
        d_threaded = run_book(d_config, &d_partition, true);
        other.join();
        same_run(c_threaded, c_solo);
        same_run(d_threaded, d_solo);
        CHECK(c_threaded.thread_owns);
        CHECK(d_threaded.thread_owns);
    }
    std::printf("threads: two hosts on two threads read their solo values\n");
}

}  // namespace

int main() {
    block_matches_thread_local_storage();
    a_host_that_never_asks_installs_nothing();
    the_block_is_a_window();
    interleaved_streams_read_their_solo_values();
    a_nested_run_reads_its_own_scopes();
    two_threads_read_their_solo_values();
    CHECK(thread_owns_its_state());
    if (tally.failures != 0) {
        std::fprintf(stderr, "test_native_runtime_ambient: %d failure(s) in %ld checks\n",
                     tally.failures, tally.checks);
        return 1;
    }
    std::printf("test_native_runtime_ambient: ok (%ld checks)\n", tally.checks);
    return 0;
}
