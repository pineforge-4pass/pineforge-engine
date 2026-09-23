// R5 lane PERF-P1: the latched continuation, taken as a VIEW and folded on
// first read, answers the eager capture's value at every read.
//
// A host latches the continuation at a script point (the engine's
// last_script_continuation_* snapshot), and broker_state_hash() folds that
// latch for as long as it stands. Until this lane the latch was one full
// continuation fold, taken at once; for a Pine run that fold is the whole
// driver log (four points a bar) and the whole command history, a fifth of
// the run, paid by every run although nothing on the benchmark or report path
// reads it. NativeExecutionConsumer::capture_continuation_view records the
// bytes the fold would consume instead, with the history and driver digests
// left as holes at the logs' lengths, and the first read folds them.
//
// Nothing about the value may move, so the witness is equality, read for
// read, between three spellings of one latch on one bare host:
//   Base      the host latches native_continuation_hash() itself -- the only
//             spelling the base library has, and the pins' provenance;
//   Eager     the consumer's view, folded at capture
//             (defer_continuation_views(false): the eager capture a view
//             reproduces);
//   Deferred  the consumer's view, folded when first read (the default).
// The reads cover what could separate them: a read long after the capture,
// with the logs grown underneath (the digests fold only their tail); a live
// native_continuation_hash() between the capture and the read, which carries
// the digests past the view's lengths, so the view has to be folded first
// (materialise before advance); a read after Completed; a KernelRecorded
// report point that rewrites the latch after a capture; an abort; a stream's
// warmup and realtime legs; a begin that is refused before it resets
// anything, which must leave the view readable; and the next begin of a
// reused host, which drops the view before it clears the logs (drop at
// begin). The Deferred leg also shows the fold was really skipped: the view
// is still pending when the run returns.
//
// Portability. The spec's zone is the fixed offset "UTC+0", which the
// resolver answers from its definition alone, so no tzdata file enters the
// digest (E23) and the pins hold on every host.
//
// Provenance of the pinned data: this TU compiled unchanged against the base
// library (engine main fc7aad62) with -DPINEFORGE_P1_HARVEST, which runs the
// Base spelling only and prints the observed reads as the initializers
// below. Rebuild them the same way; never edit one by hand.
//
// Fail-before (fc7aad62), first diagnostic without the harvest switch:
//   tests/test_native_continuation_view.cpp:102:20: error: no member named
//   'defer_continuation_views' in 'pineforge::NativeExecutionConsumer'
#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

enum class Capture { Base, Eager, Deferred };

const char* name_of(Capture capture) {
    switch (capture) {
    case Capture::Base: return "Base";
    case Capture::Eager: return "Eager";
    case Capture::Deferred: return "Deferred";
    }
    return "?";
}

bool at(const std::vector<int>& bars, int bar) {
    for (const int b : bars) if (b == bar) return true;
    return false;
}

// Every log the fold reads grows here: one working request accepted and
// cancelled a bar (command history), a market round trip every six bars
// (fills: the account log), their buys enrolled in one cohort (cohort
// receipts), a stop that stays working (the live table), and the driver log
// under all of it.
struct ViewHost final : NativeStrategyHost {
    Capture capture = Capture::Deferred;
    std::vector<int> latch_at;       // latch at the end of these bars' callbacks
    std::vector<int> read_latch_at;  // broker_state_hash(), which folds the latch
    std::vector<int> read_live_at;   // native_continuation_hash(), which folds now
    int abort_at = -1;
    std::vector<std::uint64_t> reads;
    int bar = 0;
    no::CohortHandle cohort{};

    explicit ViewHost(Capture mode) : capture(mode) {
#ifndef PINEFORGE_P1_HARVEST
        consumer().defer_continuation_views(mode != Capture::Eager);
#endif
    }

    // How often a live read found the view pending, so the Deferred leg can
    // show it walked the "materialise before advance" path.
    int materialised = 0;

    void on_native_run_begin() override {
        bar = 0;
        cohort = cohort_open();
#ifndef PINEFORGE_P1_HARVEST
        // Drop at begin: this begin has cleared the logs, so no view of the
        // previous run's latch may survive it -- a pending one would be
        // folded here against the wrong logs.
        constexpr std::uint64_t kNoView = 0x9e3779b97f4a7c15ull;
        CHECK(consumer().latched_continuation(kNoView) == kNoView);
#endif
    }

    void read_live() {
#ifndef PINEFORGE_P1_HARVEST
        const bool pending = view_pending();
#endif
        reads.push_back(native_continuation_hash());
#ifndef PINEFORGE_P1_HARVEST
        // Materialise before advance: the live fold, which carries the
        // digests to the logs' ends, folded the pending view on its way.
        if (pending) {
            ++materialised;
            CHECK(!view_pending());
        }
#endif
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        auto rest = no::Request{no::Transact{1.0}, "rest", ""};
        rest.trigger = no::Limit{1.0};
        const auto accepted = submit(rest);
        if (accepted.handle) cancel(*accepted.handle);
        if (bar % 6 == 1) {
            const auto buy = submit(no::Request{no::Transact{1.0}, "buy", ""});
            if (buy.handle) cohort_add(cohort, *buy.handle);
        }
        if (bar % 6 == 4) submit(no::Request{no::Flatten{}, "flat", ""});
        if (bar == 3) {
            auto guard = no::Request{no::Transact{-1.0}, "guard", ""};
            guard.trigger = no::Stop{1.0};
            submit(guard);
        }
        if (at(read_live_at, bar)) read_live();
        if (at(read_latch_at, bar)) reads.push_back(broker_state_hash());
        if (at(latch_at, bar)) latch();
        if (bar == abort_at) request_abort();
        ++bar;
    }

    void latch() {
#ifndef PINEFORGE_P1_HARVEST
        if (capture != Capture::Base) {
            consumer().capture_continuation_view();
            last_script_continuation_valid_ = true;
            return;
        }
#endif
        last_script_continuation_hash_ = native_continuation_hash();
        last_script_continuation_valid_ = true;
    }

    void read_now() {
        reads.push_back(broker_state_hash());
        read_live();
        reads.push_back(broker_state_hash());
    }

#ifndef PINEFORGE_P1_HARVEST
    bool view_pending() { return consumer().continuation_view_pending(); }
#endif

private:
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
};

NativeRunSpec view_spec(std::uint64_t run_number,
                        NativeReportPolicy policy = NativeReportPolicy::HostRecorded) {
    NativeRunSpec spec;
    spec.identity = {"p1-continuation-view", run_number};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:P1";
    spec.timezone = "UTC+0";
    spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 1;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    spec.report_policy = policy;
    return spec;
}

constexpr std::int64_t T = 1736121600000LL;

// Every price an exact binary fraction on the 0.25 tick.
std::vector<Bar> tape(int count, int from = 0) {
    std::vector<Bar> bars;
    for (int i = from; i < from + count; ++i) {
        const int phase = i % 8;
        const double p = 100.0 + 0.5 * (phase < 4 ? phase : 8 - phase) + 0.25 * (i % 3);
        bars.push_back({p, p + 0.5, p - 0.5, p + 0.25, 1.0,
                        T + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

constexpr int kBars = 60;

// A batch: reads long after a latch, a live read between a latch and its
// read, and reads after Completed.
void batch_schedule(ViewHost& host) {
    host.latch_at = {10, 30, kBars - 1};
    host.read_live_at = {12, 40};
    host.read_latch_at = {15, 35, 45};
}

bool configure(ViewHost& host, std::uint64_t run_number,
               NativeReportPolicy policy = NativeReportPolicy::HostRecorded) {
    const auto setup = host.configure_native(view_spec(run_number, policy));
    if (setup.status != NativeSetupStatus::Applied)
        std::fprintf(stderr, "configure refused: %s\n", host.last_error().c_str());
    CHECK(setup.status == NativeSetupStatus::Applied);
    return setup.status == NativeSetupStatus::Applied;
}

std::vector<std::uint64_t> batch(Capture mode, bool* pending_after_run = nullptr,
                                 int* materialised = nullptr) {
    ViewHost host(mode);
    batch_schedule(host);
    if (!configure(host, 1)) return {};
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
#ifndef PINEFORGE_P1_HARVEST
    if (pending_after_run) *pending_after_run = host.view_pending();
#else
    (void)pending_after_run;
#endif
    host.read_now();
    if (materialised) *materialised = host.materialised;
    return host.reads;
}

// KernelRecorded: the consumer latches at every report point, after the
// callback that latched a view, so the view must give way to it.
std::vector<std::uint64_t> kernel_recorded(Capture mode) {
    ViewHost host(mode);
    batch_schedule(host);
    host.read_latch_at = {11, 15, 31, 35};
    if (!configure(host, 1, NativeReportPolicy::KernelRecorded)) return {};
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.read_now();
    return host.reads;
}

// An abort after a latch: the run stops at its next bar, the latch stands.
std::vector<std::uint64_t> aborted(Capture mode) {
    ViewHost host(mode);
    host.latch_at = {20};
    host.read_live_at = {22};
    host.abort_at = 25;
    if (!configure(host, 1)) return {};
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    host.read_now();
    return host.reads;
}

// A stream: latches in the warmup and at realtime bars, reads between pushes.
std::vector<std::uint64_t> stream(Capture mode) {
    ViewHost host(mode);
    host.latch_at = {15, 21, 22, 23, 26, 29, 33};
    host.read_live_at = {24};
    host.read_latch_at = {18, 30};
    if (!configure(host, 1)) return {};
    const auto warmup = tape(20);
    CHECK(host.stream_begin(warmup.data(), 20, "1"));
    const auto live = tape(16, 20);
    for (std::size_t i = 0; i < live.size(); ++i) {
        CHECK(host.stream_push_bar(live[i]));
        host.reads.push_back(host.broker_state_hash());
        if (i % 3 == 0) host.read_live();
    }
    CHECK(host.stream_end());
    host.read_now();
    return host.reads;
}

// One host, three runs. The first run's view is left unread: the second
// begin drops it before it clears the logs the view names, and the second
// run reads a fresh latch. The third run follows a read of the second's.
std::vector<std::uint64_t> reused(Capture mode) {
    ViewHost host(mode);
    batch_schedule(host);
    const auto bars = tape(kBars);
    if (!configure(host, 1)) return {};
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.latch_at = {5, kBars - 1};
    host.read_latch_at = {0, 8};
    host.read_live_at = {3};
    if (!configure(host, 2)) return {};
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.read_now();
    if (!configure(host, 3)) return {};
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.read_now();
    return host.reads;
}

// A begin refused before it resets anything -- the next run's bars fail
// preflight -- leaves the run's latch standing, so its unread view has to
// read after the refusal, the live fold first. The same staged spec then
// begins over valid bars.
std::vector<std::uint64_t> refused(Capture mode) {
    ViewHost host(mode);
    batch_schedule(host);
    host.read_latch_at.clear();
    host.read_live_at.clear();
    const auto bars = tape(kBars);
    if (!configure(host, 1)) return {};
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    if (!configure(host, 2)) return {};
    auto repeated = bars;
    repeated[5].timestamp = repeated[4].timestamp;
    host.run(repeated.data(), kBars);
    CHECK(!host.last_error().empty());
    host.read_live();
    host.read_now();
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.read_now();
    return host.reads;
}

struct Scenario {
    const char* name;
    std::vector<std::uint64_t> (*run)(Capture);
};

std::vector<std::uint64_t> batch_reads(Capture mode) { return batch(mode); }

const Scenario kScenarios[] = {
    {"batch", batch_reads},
    {"kernel_recorded", kernel_recorded},
    {"aborted", aborted},
    {"stream", stream},
    {"reused", reused},
    {"refused", refused},
};

#ifdef PINEFORGE_P1_HARVEST
void emit(const char* name, const std::vector<std::uint64_t>& reads) {
    std::printf("constexpr std::uint64_t k_%s[] = {\n", name);
    for (const auto h : reads) std::printf("    %lluull,\n", static_cast<unsigned long long>(h));
    std::printf("};\n");
}
#else
// ── Pinned data (see the provenance note at the top of this file) ───────
// P1_PINNED_DATA_BEGIN
constexpr std::uint64_t k_batch[] = {
    16707671882135373688ull,
    3099669921249327852ull,
    2542166709198525756ull,
    14327396292160246865ull,
    7231218369080234634ull,
    15819661924728693342ull,
    599364476702305352ull,
    15819661924728693342ull,
};
constexpr std::uint64_t k_kernel_recorded[] = {
    5614733036278399145ull,
    11676015693735769359ull,
    5483622090882432494ull,
    13722241701431726219ull,
    4889216368716700511ull,
    14291832670492397018ull,
    1549062683499419271ull,
    8855897628712277785ull,
    1549062683499419271ull,
};
constexpr std::uint64_t k_aborted[] = {
    16067046753788442231ull,
    9514302941284171977ull,
    7079798733135204626ull,
    9514302941284171977ull,
};
constexpr std::uint64_t k_stream[] = {
    16275711105144533401ull,
    5409785957580623172ull,
    7961469203483496765ull,
    1638545468783150428ull,
    10730343412533885313ull,
    8846305283544373266ull,
    5253566024529150784ull,
    4227322838956454298ull,
    1625478086044697179ull,
    18284859851896031180ull,
    5771476474414506679ull,
    16051197728738505637ull,
    3608236266614344830ull,
    10658298854972193689ull,
    11116849820015870269ull,
    18066808579993347745ull,
    14822839943623034916ull,
    14822839943623034916ull,
    6159132903231091562ull,
    14982653080744883739ull,
    9416702128817427488ull,
    1392898661344125703ull,
    10316083322694488882ull,
    5069404992856400929ull,
    3718933773513696001ull,
    5069404992856400929ull,
    11284100477980189823ull,
    5069404992856400929ull,
};
constexpr std::uint64_t k_reused[] = {
    16707671882135373688ull,
    3099669921249327852ull,
    2542166709198525756ull,
    14327396292160246865ull,
    7231218369080234634ull,
    5881860634311433903ull,
    14056777621596896046ull,
    5633380384066313795ull,
    15819661924728693342ull,
    599364476702305352ull,
    15819661924728693342ull,
    5881860634311433903ull,
    14056777621596896046ull,
    5633380384066313795ull,
    15819661924728693342ull,
    599364476702305352ull,
    15819661924728693342ull,
};
constexpr std::uint64_t k_refused[] = {
    16363529955321928359ull,
    15819661924728693342ull,
    16363529955321928359ull,
    15819661924728693342ull,
    15819661924728693342ull,
    599364476702305352ull,
    15819661924728693342ull,
};
// P1_PINNED_DATA_END

struct Pinned {
    const std::uint64_t* reads;
    std::size_t len;
};

const Pinned kPinned[] = {
    {k_batch, std::size(k_batch)},
    {k_kernel_recorded, std::size(k_kernel_recorded)},
    {k_aborted, std::size(k_aborted)},
    {k_stream, std::size(k_stream)},
    {k_reused, std::size(k_reused)},
    {k_refused, std::size(k_refused)},
};

std::size_t distinct(const Pinned& pinned) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < pinned.len; ++i) {
        bool seen = false;
        for (std::size_t j = 0; j < i; ++j) seen = seen || pinned.reads[j] == pinned.reads[i];
        if (!seen) ++count;
    }
    return count;
}

// The first read that moved, so a failing view names the point.
void same_reads(const char* scenario, Capture mode, const std::vector<std::uint64_t>& got,
                const std::uint64_t* want, std::size_t want_len) {
    CHECK(got.size() == want_len);
    for (std::size_t i = 0; i < got.size() && i < want_len; ++i) {
        if (got[i] != want[i]) {
            std::fprintf(stderr, "%s %s read[%zu]: observed %llu, want %llu\n", scenario,
                         name_of(mode), i, static_cast<unsigned long long>(got[i]),
                         static_cast<unsigned long long>(want[i]));
            CHECK(got[i] == want[i]);
            return;
        }
    }
}
#endif

} // namespace

int main() {
#ifdef PINEFORGE_P1_HARVEST
    for (const auto& scenario : kScenarios) emit(scenario.name, scenario.run(Capture::Base));
    return 0;
#else
    for (std::size_t s = 0; s < std::size(kScenarios); ++s) {
        const auto& scenario = kScenarios[s];
        const auto base = scenario.run(Capture::Base);
        const auto eager = scenario.run(Capture::Eager);
        const auto deferred = scenario.run(Capture::Deferred);
        // The core: the deferred view answers the eager capture at every read.
        same_reads(scenario.name, Capture::Deferred, deferred, eager.data(), eager.size());
        same_reads(scenario.name, Capture::Eager, eager, base.data(), base.size());
        // And the values are the base library's, read for read.
        same_reads(scenario.name, Capture::Deferred, deferred, kPinned[s].reads, kPinned[s].len);
    }
    // The fold is really deferred: a Deferred run returns with its view
    // pending, an Eager one with nothing left to fold; and the Deferred run's
    // live read at bar 12 found the bar-10 view pending and folded it first.
    bool pending = false;
    int materialised = 0;
    batch(Capture::Deferred, &pending, &materialised);
    CHECK(pending);
    CHECK(materialised == 1);
    batch(Capture::Eager, &pending, &materialised);
    CHECK(!pending);
    CHECK(materialised == 0);
    // The pins are non-trivial: each scenario reads at least three values.
    for (const auto& pinned : kPinned) CHECK(distinct(pinned) >= 3);
    if (failures == 0)
        std::printf("test_native_continuation_view: %d checks, 0 failures\n", checks);
    return failures == 0 ? 0 : 1;
#endif
}
