// R5 lane PERF-P1, reverted to an eager latch by lane V19-A: the latched
// continuation, read for read, at every point that could separate a latch
// from the live fold.
//
// A host latches the continuation at a script point (the engine's
// last_script_continuation_* snapshot), and broker_state_hash() folds that
// latch for as long as it stands. Lane PERF-P1 took the latch as a VIEW -- the
// bytes the fold would consume, the history and driver digests left as holes
// at the logs' lengths -- and folded it on first read, because the v18 fold
// was the whole driver log and command history. Since v19
// (native-consumer/v9) the fold is the consumer's live state alone, so a view
// records as many words as the fold mixes, and the latch is taken at once
// again. The scenarios stay, pinning the latch where a view once had to be
// careful: a read long after the capture, with the logs grown underneath; a
// live native_continuation_hash() between the capture and the read; a read
// after Completed; a KernelRecorded report point that rewrites the latch after
// a capture; an abort; a stream's warmup and realtime legs; a begin that is
// refused before it resets anything; and the next begin of a reused host.
// Each scenario runs twice, and both runs read the pins.
//
// Portability. The spec's zone is the fixed offset "UTC+0", which the
// resolver answers from its definition alone, so no tzdata file enters the
// digest (E23) and the pins hold on every host.
//
// Provenance of the pinned data: this TU with -DPINEFORGE_P1_HARVEST, which
// prints the observed reads as the initializers below. PERF-P1 harvested
// them on engine main fc7aad62; lane V19-A re-pinned them once, and each
// array's note gives the old and the new value. Rebuild them the same way;
// never edit one by hand.
//
// Fail-before of PERF-P1 (fc7aad62), first diagnostic without the harvest
// switch, kept as the row's history:
//   tests/test_native_continuation_view.cpp:102:20: error: no member named
//   'defer_continuation_views' in 'pineforge::NativeExecutionConsumer'
#include <pineforge/native_host.hpp>

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
    std::vector<int> latch_at;       // latch at the end of these bars' callbacks
    std::vector<int> read_latch_at;  // broker_state_hash(), which folds the latch
    std::vector<int> read_live_at;   // native_continuation_hash(), which folds now
    int abort_at = -1;
    std::vector<std::uint64_t> reads;
    int bar = 0;
    no::CohortHandle cohort{};

    void on_native_run_begin() override {
        bar = 0;
        cohort = cohort_open();
    }

    void read_live() { reads.push_back(native_continuation_hash()); }

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
        last_script_continuation_hash_ = native_continuation_hash();
        last_script_continuation_valid_ = true;
    }

    void read_now() {
        reads.push_back(broker_state_hash());
        read_live();
        reads.push_back(broker_state_hash());
    }
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

std::vector<std::uint64_t> batch() {
    ViewHost host;
    batch_schedule(host);
    if (!configure(host, 1)) return {};
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    host.read_now();
    return host.reads;
}

// KernelRecorded: the consumer latches at every report point, after the
// callback that latched, so its latch replaces the host's.
std::vector<std::uint64_t> kernel_recorded() {
    ViewHost host;
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
std::vector<std::uint64_t> aborted() {
    ViewHost host;
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
std::vector<std::uint64_t> stream() {
    ViewHost host;
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

// One host, three runs. The first run's latch is left unread and the second
// run reads a fresh latch. The third run follows a read of the second's.
std::vector<std::uint64_t> reused() {
    ViewHost host;
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
// preflight -- leaves the run's latch standing, read after the refusal, the
// live fold first. The same staged spec then begins over valid bars.
std::vector<std::uint64_t> refused() {
    ViewHost host;
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
    std::vector<std::uint64_t> (*run)();
};

const Scenario kScenarios[] = {
    {"batch", batch},
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
// K-IDX follow-up: re-pins the v19 coordinate/input-coordinate hash witness.
// ── Pinned data (see the provenance note at the top of this file) ───────
// P1_PINNED_DATA_BEGIN
// expectation corrected (k_batch, 8 of 8 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   16707671882135373688ull -> 3874706988099123825ull
//   3099669921249327852ull -> 15367836576748901385ull
//   2542166709198525756ull -> 15981468109940909710ull
//   14327396292160246865ull -> 1574352814059675780ull
//   7231218369080234634ull -> 15523529820165440383ull
//   15819661924728693342ull -> 7430567613918907405ull
//   599364476702305352ull -> 15506168331272403514ull
//   15819661924728693342ull -> 7430567613918907405ull
constexpr std::uint64_t k_batch[] = {
    10282206035073575595ull,
    8117105284874503380ull,
    8848398979768703329ull,
    4739742555599573399ull,
    1352511348581510494ull,
    18082735004372283363ull,
    17267207790075143748ull,
    18082735004372283363ull,
};
// expectation corrected (k_kernel_recorded, 9 of 9 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   5614733036278399145ull -> 11312775735583565012ull
//   11676015693735769359ull -> 3844111693087381867ull
//   5483622090882432494ull -> 13662970706453162980ull
//   13722241701431726219ull -> 8477441320119823741ull
//   4889216368716700511ull -> 15507625209878714049ull
//   14291832670492397018ull -> 16045639815131724759ull
//   1549062683499419271ull -> 3213001586562170957ull
//   8855897628712277785ull -> 519597647876169671ull
//   1549062683499419271ull -> 3213001586562170957ull
constexpr std::uint64_t k_kernel_recorded[] = {
    4079040699515909993ull,
    8123678674598245289ull,
    12215185157324931552ull,
    17262264338911052226ull,
    12668708195714482216ull,
    10044851812123539492ull,
    616537690244417935ull,
    14571142726868795127ull,
    616537690244417935ull,
};
// expectation corrected (k_aborted, 4 of 4 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   16067046753788442231ull -> 14046605782901906062ull
//   9514302941284171977ull -> 16254129914904921141ull
//   7079798733135204626ull -> 1295792807353581922ull
//   9514302941284171977ull -> 16254129914904921141ull
constexpr std::uint64_t k_aborted[] = {
    1685959831200146148ull,
    2777208674932158338ull,
    7516043338442549931ull,
    2777208674932158338ull,
};
// expectation corrected (k_stream, 28 of 28 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   16275711105144533401ull -> 15937882828208296805ull
//   5409785957580623172ull -> 11432425108529535082ull
//   7961469203483496765ull -> 16673418515298261931ull
//   1638545468783150428ull -> 5285458039005469452ull
//   10730343412533885313ull -> 18056941387629449960ull
//   8846305283544373266ull -> 2894462375496212427ull
//   5253566024529150784ull -> 14624429446677514850ull
//   4227322838956454298ull -> 2112674620346426867ull
//   1625478086044697179ull -> 2562611748235069338ull
//   18284859851896031180ull -> 5998374266004261071ull
//   5771476474414506679ull -> 13898182161176238454ull
//   16051197728738505637ull -> 14276627538157298320ull
//   3608236266614344830ull -> 17794722356796227927ull
//   10658298854972193689ull -> 8310987042865327773ull
//   11116849820015870269ull -> 12354683111785255661ull
//   18066808579993347745ull -> 3313601104948653732ull
//   14822839943623034916ull -> 3725435773589134000ull
//   14822839943623034916ull -> 3725435773589134000ull
//   6159132903231091562ull -> 10233812493322420892ull
//   14982653080744883739ull -> 8747004096016098601ull
//   9416702128817427488ull -> 761589802742250794ull
//   1392898661344125703ull -> 4152804575051141009ull
//   10316083322694488882ull -> 10104621249650088517ull
//   5069404992856400929ull -> 11817631875067567722ull
//   3718933773513696001ull -> 13519590318807349176ull
//   5069404992856400929ull -> 11817631875067567722ull
//   11284100477980189823ull -> 213879106190249824ull
//   5069404992856400929ull -> 11817631875067567722ull
constexpr std::uint64_t k_stream[] = {
    9776873597807195832ull,
    11986069253580236917ull,
    17162291834872339403ull,
    13786842037793151004ull,
    8447372653719566623ull,
    17626625212449675552ull,
    8229491038000045297ull,
    10072266054313648201ull,
    3197159470215357145ull,
    10620998501987806702ull,
    12624016652927674722ull,
    16556044180419270596ull,
    16518141588396736915ull,
    13799343351947421097ull,
    16028032644108071858ull,
    16971605674914249940ull,
    2093050977296283999ull,
    2093050977296283999ull,
    2581144724447198593ull,
    10907065441645508020ull,
    8461248817837216019ull,
    16002953856556132057ull,
    14714968428239803997ull,
    1342907117657859650ull,
    10370341518995788703ull,
    1342907117657859650ull,
    15555197184326437069ull,
    1342907117657859650ull,
};
// expectation corrected (k_reused, 17 of 17 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   16707671882135373688ull -> 3874706988099123825ull
//   3099669921249327852ull -> 15367836576748901385ull
//   2542166709198525756ull -> 15981468109940909710ull
//   14327396292160246865ull -> 1574352814059675780ull
//   7231218369080234634ull -> 15523529820165440383ull
//   5881860634311433903ull -> 9335691473124626521ull
//   14056777621596896046ull -> 15291595157536991983ull
//   5633380384066313795ull -> 11095210267434077328ull
//   15819661924728693342ull -> 7430567613918907405ull
//   599364476702305352ull -> 15506168331272403514ull
//   15819661924728693342ull -> 7430567613918907405ull
//   5881860634311433903ull -> 9335691473124626521ull
//   14056777621596896046ull -> 15291595157536991983ull
//   5633380384066313795ull -> 11095210267434077328ull
//   15819661924728693342ull -> 7430567613918907405ull
//   599364476702305352ull -> 15506168331272403514ull
//   15819661924728693342ull -> 7430567613918907405ull
constexpr std::uint64_t k_reused[] = {
    10282206035073575595ull,
    8117105284874503380ull,
    8848398979768703329ull,
    4739742555599573399ull,
    1352511348581510494ull,
    6763619293117683629ull,
    2018836319815576259ull,
    3214984387103191743ull,
    18082735004372283363ull,
    17267207790075143748ull,
    18082735004372283363ull,
    6763619293117683629ull,
    2018836319815576259ull,
    3214984387103191743ull,
    18082735004372283363ull,
    17267207790075143748ull,
    18082735004372283363ull,
};
// expectation corrected (k_refused, 7 of 7 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   16363529955321928359ull -> 9631061672234698124ull
//   15819661924728693342ull -> 7430567613918907405ull
//   16363529955321928359ull -> 9631061672234698124ull
//   15819661924728693342ull -> 7430567613918907405ull
//   15819661924728693342ull -> 7430567613918907405ull
//   599364476702305352ull -> 15506168331272403514ull
//   15819661924728693342ull -> 7430567613918907405ull
constexpr std::uint64_t k_refused[] = {
    2939658939248907784ull,
    18082735004372283363ull,
    2939658939248907784ull,
    18082735004372283363ull,
    18082735004372283363ull,
    17267207790075143748ull,
    18082735004372283363ull,
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

// The first read that moved, so a failing latch names the point.
void same_reads(const char* scenario, int leg, const std::vector<std::uint64_t>& got,
                const std::uint64_t* want, std::size_t want_len) {
    CHECK(got.size() == want_len);
    for (std::size_t i = 0; i < got.size() && i < want_len; ++i) {
        if (got[i] != want[i]) {
            std::fprintf(stderr, "%s run %d read[%zu]: observed %llu, want %llu\n", scenario,
                         leg, i, static_cast<unsigned long long>(got[i]),
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
    for (const auto& scenario : kScenarios) emit(scenario.name, scenario.run());
    return 0;
#else
    for (std::size_t s = 0; s < std::size(kScenarios); ++s) {
        const auto& scenario = kScenarios[s];
        for (int leg = 1; leg <= 2; ++leg)
            same_reads(scenario.name, leg, scenario.run(), kPinned[s].reads, kPinned[s].len);
    }
    // The pins are non-trivial: each scenario reads at least three values.
    for (const auto& pinned : kPinned) CHECK(distinct(pinned) >= 3);
    if (failures == 0)
        std::printf("test_native_continuation_view: %d checks, 0 failures\n", checks);
    return failures == 0 ? 0 : 1;
#endif
}
