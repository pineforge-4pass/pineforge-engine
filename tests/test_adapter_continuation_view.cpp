// R5 lane PERF-P1, reverted to an eager latch by lane V19-A: a Pine run's
// terminal continuation at every capture site, read for read.
//
// PineStrategyHost::capture_script_continuation_hash latches the
// continuation at the run's last script point -- the last batch bar, a
// leftover input after an aggregated chart's last bucket, every realtime bar
// of a stream -- so that broker_state_hash() is one value with recording on
// or off. Lane PERF-P1 took that latch as a view of the fold, folded on first
// read, because the v18 fold was the whole driver log and command history.
// Since v19 (native-consumer/v9) the fold is the consumer's live state alone,
// so a view records as many words as the fold mixes, and the capture is
// eager again.
//
// The witness runs every scenario twice and asserts that both runs read the
// pins: reads from inside the script (the live fold and the latch), after
// Completed, after the next begin of a reused host, after a refused begin and
// after an abort; the recorded rows too, under recording. The drivings cover
// every capture site: chart timeframe, bar magnifier, an aggregated chart
// with leftover input (with and without the magnifier), calc_on_order_fills,
// recording on, a stream's warmup and realtime legs, and a stream that aborts.
//
// Portability. The syminfo zone is the fixed offset "UTC+0", which the
// resolver answers from its definition alone, so no tzdata file enters the
// continuation (E23) and the pins hold on every host. Every price is an exact
// binary fraction on the 0.25 tick.
//
// Provenance of the pinned data: this TU with -DPINEFORGE_P1_HARVEST, which
// prints the observed values as the initializers below. PERF-P1 harvested
// them on engine main fc7aad62; lane V19-A re-pinned them once, and each
// array's note gives the old and the new values. Rebuild them the same way;
// never edit one by hand.
//
// Fail-before of PERF-P1 (fc7aad62), first diagnostic without the harvest
// switch, kept as the row's history:
//   tests/test_adapter_continuation_view.cpp:106:20: error: no member named
//   'defer_continuation_views' in 'pineforge::NativeExecutionConsumer'
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>

namespace {
using namespace pineforge;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::int64_t T = 1736121600000LL;

bool at(const std::vector<int>& bars, int bar) {
    for (const int b : bars) if (b == bar) return true;
    return false;
}

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

struct Options {
    bool recording = false;
    bool coof = false;
};

// A generated-strategy-shaped source host: entries, a bracket, a resting
// order and its cancel, a close -- the command history, the fills and the
// cohorts a Pine run folds -- and reads of the live fold and of the latch.
class ViewStrategy final : public source::PineStrategyHost {
public:
    explicit ViewStrategy(const Options& options) {
        set_syminfo_timezone("UTC+0");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 1.0;
        config.calc_on_order_fills = options.coof;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(options.recording);
    }

    std::vector<int> read_latch_at;
    std::vector<int> read_live_at;
    int abort_at = -1;
    std::vector<std::uint64_t> reads;

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i % 8 == 1) strategy_entry("L", true);
        if (i % 8 == 2) strategy_exit("x", "L", bar.close + 1.0, bar.close - 1.0);
        if (i % 8 == 3) strategy_order("o", true, 1, bar.close - 2.0);
        if (i % 8 == 5) strategy_cancel("o");
        if (i % 16 == 6) strategy_entry("S", false, bar.close + 0.25);
        if (i % 16 == 12) strategy_close_all();
        if (at(read_live_at, i)) reads.push_back(native_continuation_hash());
        if (at(read_latch_at, i)) reads.push_back(broker_state_hash());
        if (i == abort_at) request_abort();
    }

    void read_now() {
        reads.push_back(broker_state_hash());
        reads.push_back(native_continuation_hash());
        reads.push_back(broker_state_hash());
    }

    void read_rows() {
        ReportC report{};
        fill_report(&report);
        for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
            reads.push_back(report.broker_state_hash[i]);
        reads.push_back(static_cast<std::uint64_t>(report.trades_len));
        BacktestEngine::free_report(&report);
    }
};

constexpr int kBars = 64;

void script_reads(ViewStrategy& host) {
    host.read_live_at = {10, 30};
    host.read_latch_at = {20, 50};
}

std::vector<std::uint64_t> chart(const Options& options) {
    ViewStrategy host(options);
    script_reads(host);
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.read_now();
    return host.reads;
}

std::vector<std::uint64_t> magnifier(const Options& options) {
    ViewStrategy host(options);
    script_reads(host);
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars, "1", "1", true, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    host.read_now();
    return host.reads;
}

// 63 one-minute inputs on a five-minute chart: three inputs arrive after the
// last full bucket, and the latch is taken again on each.
std::vector<std::uint64_t> aggregated(const Options& options, bool with_magnifier) {
    ViewStrategy host(options);
    host.read_live_at = {3};
    host.read_latch_at = {8};
    const auto bars = tape(kBars - 1);
    host.run(bars.data(), kBars - 1, "1", "5", with_magnifier, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    host.read_now();
    return host.reads;
}

std::vector<std::uint64_t> aggregated_chart(const Options& options) {
    return aggregated(options, false);
}

std::vector<std::uint64_t> aggregated_magnifier(const Options& options) {
    return aggregated(options, true);
}

std::vector<std::uint64_t> coof(const Options& options) {
    Options with = options;
    with.coof = true;
    return chart(with);
}

// Recording on: the scalar and every row agree.
std::vector<std::uint64_t> recording(const Options& options) {
    Options with = options;
    with.recording = true;
    ViewStrategy host(with);
    script_reads(host);
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.read_now();
    host.read_rows();
    return host.reads;
}

// A stream latches at every realtime bar: the reads between pushes and from
// inside the script read the latch, some after a live fold.
std::vector<std::uint64_t> stream(const Options& options, int abort_at) {
    ViewStrategy host(options);
    host.read_live_at = {36, 45};
    host.read_latch_at = {33, 40, 46};
    host.abort_at = abort_at;
    const auto warmup = tape(30);
    CHECK(host.stream_begin(warmup.data(), 30, "1"));
    const auto live = tape(34, 30);
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (!host.stream_push_bar(live[i])) break;
        host.reads.push_back(host.broker_state_hash());
        if (i % 3 == 0) host.reads.push_back(host.native_continuation_hash());
    }
    host.stream_end();
    host.read_now();
    return host.reads;
}

std::vector<std::uint64_t> streamed(const Options& options) { return stream(options, -1); }

std::vector<std::uint64_t> stream_aborted(const Options& options) {
    return stream(options, 50);
}

std::vector<std::uint64_t> aborted(const Options& options) {
    ViewStrategy host(options);
    script_reads(host);
    host.abort_at = 40;
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    host.read_now();
    return host.reads;
}

// One host: a batch whose latch is never read (the next begin resets it), a
// batch read after Completed, then a stream on the same host.
std::vector<std::uint64_t> reused(const Options& options) {
    ViewStrategy host(options);
    script_reads(host);
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.read_now();
    host.read_live_at.clear();
    host.read_latch_at = {35};
    const auto warmup = tape(30);
    CHECK(host.stream_begin(warmup.data(), 30, "1"));
    const auto live = tape(10, 30);
    for (const auto& bar : live) CHECK(host.stream_push_bar(bar));
    CHECK(host.stream_end());
    host.read_now();
    return host.reads;
}

// A begin refused before it resets anything leaves the latch standing: it
// reads after the refusal, the live fold first; then the host runs again.
std::vector<std::uint64_t> refused(const Options& options) {
    ViewStrategy host(options);
    const auto bars = tape(kBars);
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    auto repeated = bars;
    repeated[5].timestamp = repeated[4].timestamp;
    host.run(repeated.data(), kBars);
    CHECK(!host.last_error().empty());
    host.reads.push_back(host.native_continuation_hash());
    host.read_now();
    script_reads(host);
    host.run(bars.data(), kBars);
    CHECK(host.last_error().empty());
    host.read_now();
    return host.reads;
}

struct Scenario {
    const char* name;
    std::vector<std::uint64_t> (*run)(const Options&);
};

const Scenario kScenarios[] = {
    {"chart", chart},
    {"magnifier", magnifier},
    {"aggregated", aggregated_chart},
    {"aggregated_magnifier", aggregated_magnifier},
    {"coof", coof},
    {"recording", recording},
    {"stream", streamed},
    {"stream_aborted", stream_aborted},
    {"aborted", aborted},
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
// expectation corrected (k_chart, 7 of 7 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   2196150024938657215ull -> 2543779130970217232ull
//   12315990772160430197ull -> 5674505149482193397ull
//   17118064270458636817ull -> 9408409355545209490ull
//   16843348278421860698ull -> 12820592968519298463ull
//   7897887485017793412ull -> 4834660818464838173ull
//   9578444851499107596ull -> 5623648009772522091ull
//   7897887485017793412ull -> 4834660818464838173ull
// expectation corrected (v19-B, k_chart, 6 of 7 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   5674505149482193397ull -> 18435485463499933528ull
//   9408409355545209490ull -> 17904934979399943420ull
//   12820592968519298463ull -> 14580145596766604804ull
//   4834660818464838173ull -> 12763565106297155564ull
//   5623648009772522091ull -> 10324478866479362969ull
//   4834660818464838173ull -> 12763565106297155564ull
// expectation corrected (v19-E, k_chart, 4 of 7 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   18435485463499933528ull -> 2482407147974245352ull
//   14580145596766604804ull -> 7253611708149050520ull
//   12763565106297155564ull -> 13208968445305919179ull
//   12763565106297155564ull -> 13208968445305919179ull
// expectation corrected (V19-FIX, k_chart, 7 of 7 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   2543779130970217232ull -> 14621553252499744072ull
//   2482407147974245352ull -> 1560069015253079270ull
//   17904934979399943420ull -> 8181232920474966978ull
//   7253611708149050520ull -> 8150352125352055815ull
//   13208968445305919179ull -> 4370795498617211977ull
//   10324478866479362969ull -> 10361542891827113526ull
//   13208968445305919179ull -> 4370795498617211977ull
// expectation corrected (INT25, k_chart, 7 of 7 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   14621553252499744072ull -> 5575980828577033360ull
//   1560069015253079270ull -> 17778912298194596574ull
//   8181232920474966978ull -> 16463834114134690168ull
//   8150352125352055815ull -> 8941018855024971093ull
//   4370795498617211977ull -> 2898399952537902865ull
//   10361542891827113526ull -> 8964175343669296125ull
//   4370795498617211977ull -> 2898399952537902865ull
// expectation corrected (INT26 v19 hash re-pin, k_chart, 4 of 7 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   17778912298194596574ull -> 2540222023230692795ull [H-THIN]
//   8941018855024971093ull -> 7277266162097189602ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
constexpr std::uint64_t k_chart[] = {
    5575980828577033360ull,
    2540222023230692795ull,
    16463834114134690168ull,
    7277266162097189602ull,
    13929697350787785222ull,
    8964175343669296125ull,
    13929697350787785222ull,
};
// expectation corrected (k_magnifier, 7 of 7 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   10072028075855707823ull -> 2055499121853794215ull
//   8569312294347288489ull -> 703801249040575285ull
//   2632522011360028001ull -> 14872490446707053531ull
//   9681067599565431733ull -> 7510587436020870414ull
//   6545773095595780937ull -> 13457869248821340102ull
//   13485101370296523681ull -> 4055821710073734035ull
//   6545773095595780937ull -> 13457869248821340102ull
// expectation corrected (v19-B, k_magnifier, 6 of 7 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   703801249040575285ull -> 8642924642165389439ull
//   14872490446707053531ull -> 12633570209086223403ull
//   7510587436020870414ull -> 17362997550053029786ull
//   13457869248821340102ull -> 1693323071214726469ull
//   4055821710073734035ull -> 11201307460949430058ull
//   13457869248821340102ull -> 1693323071214726469ull
// expectation corrected (v19-E, k_magnifier, 4 of 7 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   8642924642165389439ull -> 14570450431216633494ull
//   17362997550053029786ull -> 3037174366487220809ull
//   1693323071214726469ull -> 4411147221463257853ull
//   1693323071214726469ull -> 4411147221463257853ull
// expectation corrected (V19-FIX, k_magnifier, 7 of 7 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   2055499121853794215ull -> 1310098789583311035ull
//   14570450431216633494ull -> 9252967425603149690ull
//   12633570209086223403ull -> 9717019523752667192ull
//   3037174366487220809ull -> 14284557384012157011ull
//   4411147221463257853ull -> 15984354419168920510ull
//   11201307460949430058ull -> 16607038826027722339ull
//   4411147221463257853ull -> 15984354419168920510ull
// expectation corrected (INT25, k_magnifier, 7 of 7 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   1310098789583311035ull -> 4430826578646987126ull
//   9252967425603149690ull -> 14738067743722080804ull
//   9717019523752667192ull -> 2077753189921542836ull
//   14284557384012157011ull -> 2917741806179085516ull
//   15984354419168920510ull -> 13581436790443683042ull
//   16607038826027722339ull -> 14737345821341801219ull
//   15984354419168920510ull -> 13581436790443683042ull
// expectation corrected (INT26 v19 hash re-pin, k_magnifier, 4 of 7 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   14738067743722080804ull -> 10187142414804555211ull [H-THIN]
//   2917741806179085516ull -> 13316113226488762715ull [H-THIN]
//   13581436790443683042ull -> 489358674406674971ull [H-THIN]
//   13581436790443683042ull -> 489358674406674971ull [H-THIN]
constexpr std::uint64_t k_magnifier[] = {
    4430826578646987126ull,
    10187142414804555211ull,
    2077753189921542836ull,
    13316113226488762715ull,
    489358674406674971ull,
    14737345821341801219ull,
    489358674406674971ull,
};
// expectation corrected (k_aggregated, 5 of 5 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   15579341700060222430ull -> 7408940017323371281ull
//   14994859925119476497ull -> 17786335956283624301ull
//   16532888688512789671ull -> 4311320861969680288ull
//   3620459343021292889ull -> 5456021430637490595ull
//   16532888688512789671ull -> 4311320861969680288ull
// expectation corrected (v19-E, k_aggregated, 3 of 5 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   17786335956283624301ull -> 10940756508045292790ull
//   4311320861969680288ull -> 5992953984750558008ull
//   4311320861969680288ull -> 5992953984750558008ull
// expectation corrected (V19-FIX, k_aggregated, 4 of 5 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   10940756508045292790ull -> 11387000028137044299ull
//   5992953984750558008ull -> 4995545673543620333ull
//   5456021430637490595ull -> 9893483468656556008ull
//   5992953984750558008ull -> 4995545673543620333ull
// expectation corrected (INT25, k_aggregated, 5 of 5 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   7408940017323371281ull -> 16542564830293500828ull
//   11387000028137044299ull -> 13031512803108236715ull
//   4995545673543620333ull -> 6906214955679447270ull
//   9893483468656556008ull -> 10543459672655655434ull
//   4995545673543620333ull -> 6906214955679447270ull
// expectation corrected (INT26 v19 hash re-pin, k_aggregated, 3 of 5 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   13031512803108236715ull -> 12703961494282243636ull [H-THIN]
//   6906214955679447270ull -> 4636245846172056503ull [H-THIN]
//   6906214955679447270ull -> 4636245846172056503ull [H-THIN]
constexpr std::uint64_t k_aggregated[] = {
    16542564830293500828ull,
    12703961494282243636ull,
    4636245846172056503ull,
    10543459672655655434ull,
    4636245846172056503ull,
};
// expectation corrected (k_aggregated_magnifier, 5 of 5 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   12594241837591534357ull -> 2376229218543853263ull
//   10703578186271721676ull -> 11362706686723722160ull
//   16309698280952568764ull -> 8515292501914737744ull
//   2257923986347215325ull -> 13595293692156698476ull
//   16309698280952568764ull -> 8515292501914737744ull
// expectation corrected (v19-E, k_aggregated_magnifier, 3 of 5 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   11362706686723722160ull -> 2877250321201605922ull
//   8515292501914737744ull -> 18112393076487589516ull
//   8515292501914737744ull -> 18112393076487589516ull
// expectation corrected (V19-FIX, k_aggregated_magnifier, 4 of 5 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   2877250321201605922ull -> 301774797075971540ull
//   18112393076487589516ull -> 6106527879897255797ull
//   13595293692156698476ull -> 14842205570428090150ull
//   18112393076487589516ull -> 6106527879897255797ull
// expectation corrected (INT25, k_aggregated_magnifier, 5 of 5 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   2376229218543853263ull -> 14031106247497767919ull
//   301774797075971540ull -> 10607093485200722581ull
//   6106527879897255797ull -> 22607172627576334ull
//   14842205570428090150ull -> 12265832737220737843ull
//   6106527879897255797ull -> 22607172627576334ull
// expectation corrected (PAR-ORDERS, k_aggregated_magnifier, 3 of 5 values), because R5 lane PAR-ORDERS dates every fill of an aggregated magnified run at its chart bar's open, as TradingView does, and the lots' dated times are Pine hash inputs; harvested with this TU's harvest switch on the lane's tree (a second harvest reproduces it):
//   10607093485200722581ull -> 7363580706773703120ull
//   22607172627576334ull -> 5380311451522306220ull
//   22607172627576334ull -> 5380311451522306220ull
// expectation corrected (INT26 v19 hash re-pin, k_aggregated_magnifier, 3 of 5 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   7363580706773703120ull -> 6822321611223800121ull [H-THIN]
//   5380311451522306220ull -> 1968062868302397607ull [H-THIN]
//   5380311451522306220ull -> 1968062868302397607ull [H-THIN]
constexpr std::uint64_t k_aggregated_magnifier[] = {
    14031106247497767919ull,
    6822321611223800121ull,
    1968062868302397607ull,
    12265832737220737843ull,
    1968062868302397607ull,
};
// expectation corrected (k_coof, 10 of 10 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   10454171698854540065ull -> 8160477046829279565ull
//   6158913543940850586ull -> 8683725853452377701ull
//   13904370335648497003ull -> 16612330879755983674ull
//   2471999166045289788ull -> 5948900213206369390ull
//   3344013152352926966ull -> 3940848663703711224ull
//   14244110802762791199ull -> 5449918228566479877ull
//   1854589002122406406ull -> 6135396872829398814ull
//   11650103996428590422ull -> 7598518073669438847ull
//   14620914431497623785ull -> 6938517794080095754ull
//   11650103996428590422ull -> 7598518073669438847ull
// expectation corrected (v19-B, k_coof, 7 of 10 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   5948900213206369390ull -> 10071915686754406472ull
//   3940848663703711224ull -> 9898269111674054383ull
//   5449918228566479877ull -> 12036057909661298712ull
//   6135396872829398814ull -> 4634335044637977031ull
//   7598518073669438847ull -> 15964838513564229050ull
//   6938517794080095754ull -> 17202960931486753898ull
//   7598518073669438847ull -> 15964838513564229050ull
// expectation corrected (v19-E, k_coof, 5 of 10 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   10071915686754406472ull -> 3284793633481963595ull
//   12036057909661298712ull -> 15200193571573177943ull
//   4634335044637977031ull -> 220118684227198753ull
//   15964838513564229050ull -> 5365537423784642334ull
//   15964838513564229050ull -> 5365537423784642334ull
// expectation corrected (V19-FIX, k_coof, 10 of 10 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   8160477046829279565ull -> 10706142720152936082ull
//   8683725853452377701ull -> 14042469651802437968ull
//   16612330879755983674ull -> 1992762524201176782ull
//   3284793633481963595ull -> 9972018108613272455ull
//   9898269111674054383ull -> 9011525226083087013ull
//   15200193571573177943ull -> 7755254022538140640ull
//   220118684227198753ull -> 16056359684121154720ull
//   5365537423784642334ull -> 10916717879615654635ull
//   17202960931486753898ull -> 17916071608255404005ull
//   5365537423784642334ull -> 10916717879615654635ull
// expectation corrected (INT25, k_coof, 10 of 10 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   10706142720152936082ull -> 16820608513549156800ull
//   14042469651802437968ull -> 9462991605602344175ull
//   1992762524201176782ull -> 12220673217690837314ull
//   9972018108613272455ull -> 1856715264914641232ull
//   9011525226083087013ull -> 12811765853164186626ull
//   7755254022538140640ull -> 9115044940243703863ull
//   16056359684121154720ull -> 9198850018143016781ull
//   10916717879615654635ull -> 784137025905909089ull
//   17916071608255404005ull -> 11981249830754474578ull
//   10916717879615654635ull -> 784137025905909089ull
// expectation corrected (INT26 v19 hash re-pin, k_coof, 5 of 10 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   1856715264914641232ull -> 17725898018428294595ull [H-THIN]
//   9115044940243703863ull -> 8234685298969568092ull [PAR-ORDERS-2, H-THIN]
//   9198850018143016781ull -> 13907186164657073030ull [H-THIN]
//   784137025905909089ull -> 5915100548425272856ull [H-THIN]
//   784137025905909089ull -> 5915100548425272856ull [H-THIN]
constexpr std::uint64_t k_coof[] = {
    16820608513549156800ull,
    9462991605602344175ull,
    12220673217690837314ull,
    17725898018428294595ull,
    12811765853164186626ull,
    8234685298969568092ull,
    13907186164657073030ull,
    5915100548425272856ull,
    11981249830754474578ull,
    5915100548425272856ull,
};
// expectation corrected (k_recording, 71 of 72 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   2196150024938657215ull -> 2543779130970217232ull
//   10583075953020668459ull -> 12102871947415603019ull
//   17118064270458636817ull -> 9408409355545209490ull
//   1950613932863574997ull -> 9731921227394455728ull
//   7897887485017793412ull -> 4834660818464838173ull
//   9578444851499107596ull -> 5623648009772522091ull
//   7897887485017793412ull -> 4834660818464838173ull
//   10512070055982876449ull -> 7390183078382216635ull
//   7551897722125301549ull -> 4275052578147536997ull
//   1370579259074096097ull -> 9782579203668197445ull
//   8586574392596045646ull -> 18137986040855890756ull
//   2431120502114604660ull -> 1127752528809105344ull
//   17313518311859819421ull -> 460621270365484909ull
//   3042062362875710862ull -> 10729940105119008355ull
//   992493725526045794ull -> 6719506549743237672ull
//   14348480854437341010ull -> 9601497081148438203ull
//   18133812775101086530ull -> 10071572065205276794ull
//   968774177461625230ull -> 7652585746495678617ull
//   16009040168723594381ull -> 5510227761230738907ull
//   15692042377145935477ull -> 6482743127232178518ull
//   13249088986932203475ull -> 9757172817451018200ull
//   12536366136322614067ull -> 11782218145116696034ull
//   16446987106187059362ull -> 4331588667985227456ull
//   5490715618351354652ull -> 4031221698890367954ull
//   8880982811479354810ull -> 7316513615807483167ull
//   15301670425260559618ull -> 7887806825697949620ull
//   10820127730864423231ull -> 14369960460099008479ull
//   2167857826484370020ull -> 1445832673687625444ull
//   7891589492420585092ull -> 3065788277782737739ull
//   6056408673923967935ull -> 16777199233526513054ull
//   11266758787374383672ull -> 12817867269153787474ull
//   10493222880035031315ull -> 9059503499681960574ull
//   5502789759121567448ull -> 13426438431872780295ull
//   7928074773257266694ull -> 13422469892124742512ull
//   14608702888257592135ull -> 2796634976022197618ull
//   869928054170613560ull -> 7177790782070250822ull
//   1637934069574498173ull -> 12809920657886367582ull
//   8155469509608986000ull -> 6026131998021598350ull
//   16250513059678006214ull -> 14582561950966654444ull
//   4943768578009137176ull -> 8404845127637694823ull
//   15835307311019990386ull -> 17365191549808574849ull
//   14270828729978042123ull -> 13491912311498866261ull
//   17269370295337121007ull -> 11277843155689476212ull
//   3613286801139235984ull -> 9811981650000812306ull
//   8054881784165105296ull -> 13216014769651389651ull
//   3412232190641751664ull -> 12670664983955043063ull
//   5145146943872513055ull -> 12211597308880478155ull
//   16954702499091004704ull -> 11647447651341082476ull
//   7792243328316214242ull -> 2575673101403584766ull
//   725359005093946371ull -> 2026106225486543071ull
//   6257045899022186131ull -> 17977577869004360273ull
//   4512099203943567183ull -> 2648136582573533333ull
//   8660506180503927284ull -> 4474418605383541641ull
//   1153909597881535350ull -> 738361606928178595ull
//   13671576035848622549ull -> 4322246313805691111ull
//   13482821555512783775ull -> 17334629200554089976ull
//   12351846122348314010ull -> 8098927344052802931ull
//   11078826772414619415ull -> 1380943936535676434ull
//   8766546841834001861ull -> 14453082372026923489ull
//   14701189601170319342ull -> 4600079008853518937ull
//   206875772960725182ull -> 3786255565884902157ull
//   6384811281770149723ull -> 969620699942930924ull
//   10276250789515246810ull -> 15715946055027507243ull
//   10496478071912266835ull -> 14464190903244952662ull
//   11027594364870005885ull -> 5527657694788709574ull
//   2368788186616374084ull -> 5715618086727856832ull
//   12848531737344944826ull -> 4457180920033346810ull
//   8562343566734055609ull -> 7239452625558369345ull
//   3794724587088289667ull -> 17262674001658265772ull
//   1820297518517296672ull -> 14147667235407649041ull
//   7897887485017793412ull -> 4834660818464838173ull
// expectation corrected (v19-B, k_recording, 52 of 72 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   12102871947415603019ull -> 6045399572005464099ull
//   9408409355545209490ull -> 17904934979399943420ull
//   9731921227394455728ull -> 7819774834736132418ull
//   4834660818464838173ull -> 12763565106297155564ull
//   5623648009772522091ull -> 10324478866479362969ull
//   4834660818464838173ull -> 12763565106297155564ull
//   7887806825697949620ull -> 9559358477699809697ull
//   14369960460099008479ull -> 5840017798725418135ull
//   1445832673687625444ull -> 3891810250380278777ull
//   3065788277782737739ull -> 1621842784445521476ull
//   16777199233526513054ull -> 15305619979492529468ull
//   12817867269153787474ull -> 15664724266306034558ull
//   9059503499681960574ull -> 4047121317955922549ull
//   13426438431872780295ull -> 8297527873607835451ull
//   13422469892124742512ull -> 4735701892803300920ull
//   2796634976022197618ull -> 8063401603116370931ull
//   7177790782070250822ull -> 18294135471370528909ull
//   12809920657886367582ull -> 16661456640802028241ull
//   6026131998021598350ull -> 14772433093186204495ull
//   14582561950966654444ull -> 7620644898991603805ull
//   8404845127637694823ull -> 15118496304273749831ull
//   17365191549808574849ull -> 7230774509547727832ull
//   13491912311498866261ull -> 9367416904717233797ull
//   11277843155689476212ull -> 29939079855673758ull
//   9811981650000812306ull -> 17645391567178045076ull
//   13216014769651389651ull -> 4152792192432616558ull
//   12670664983955043063ull -> 1290992315178131787ull
//   12211597308880478155ull -> 11629813004726792644ull
//   11647447651341082476ull -> 14092163688413375773ull
//   2575673101403584766ull -> 2250285108640531229ull
//   2026106225486543071ull -> 8323804787620377009ull
//   17977577869004360273ull -> 12084061299001209834ull
//   2648136582573533333ull -> 6575350347839890721ull
//   4474418605383541641ull -> 8511425819615280004ull
//   738361606928178595ull -> 3716074811611924003ull
//   4322246313805691111ull -> 17462716235150650529ull
//   17334629200554089976ull -> 14424451358928265509ull
//   8098927344052802931ull -> 8812190484420866093ull
//   1380943936535676434ull -> 8378584172590769377ull
//   14453082372026923489ull -> 12342327263055300482ull
//   4600079008853518937ull -> 5755733331066026585ull
//   3786255565884902157ull -> 11985055179349118870ull
//   969620699942930924ull -> 1729850769323221596ull
//   15715946055027507243ull -> 8374018043578414321ull
//   14464190903244952662ull -> 10405234474490090389ull
//   5527657694788709574ull -> 18200276816877882479ull
//   5715618086727856832ull -> 6678532068539452832ull
//   4457180920033346810ull -> 7638978804204415889ull
//   7239452625558369345ull -> 1592570382804466430ull
//   17262674001658265772ull -> 4258395109503891217ull
//   14147667235407649041ull -> 14202785402043642137ull
//   4834660818464838173ull -> 12763565106297155564ull
// expectation corrected (v19-E, k_recording, 68 of 72 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   6045399572005464099ull -> 13638291719838344805ull
//   7819774834736132418ull -> 4916994404502736826ull
//   12763565106297155564ull -> 13208968445305919179ull
//   12763565106297155564ull -> 13208968445305919179ull
//   7390183078382216635ull -> 10364728431969872992ull
//   4275052578147536997ull -> 6773723984513471634ull
//   9782579203668197445ull -> 17279077540384140970ull
//   18137986040855890756ull -> 273555295542222474ull
//   1127752528809105344ull -> 8791634334263737749ull
//   460621270365484909ull -> 925668679100948855ull
//   10729940105119008355ull -> 8777619534496248594ull
//   6719506549743237672ull -> 17725129497555317017ull
//   9601497081148438203ull -> 8008837844251753449ull
//   10071572065205276794ull -> 5594741258697960485ull
//   7652585746495678617ull -> 15079181317456407482ull
//   5510227761230738907ull -> 17089349116617098209ull
//   6482743127232178518ull -> 18200208521056000396ull
//   9757172817451018200ull -> 15241045489297703683ull
//   11782218145116696034ull -> 1762199573452402758ull
//   4331588667985227456ull -> 11385708691232883797ull
//   4031221698890367954ull -> 17964225683996893310ull
//   7316513615807483167ull -> 4534429732864628476ull
//   9559358477699809697ull -> 17467596193696644527ull
//   5840017798725418135ull -> 16401650137151877883ull
//   3891810250380278777ull -> 16808577086136594190ull
//   1621842784445521476ull -> 12964209703146465173ull
//   15305619979492529468ull -> 16463685165528796935ull
//   15664724266306034558ull -> 17915487794021341896ull
//   4047121317955922549ull -> 15978106025216492785ull
//   8297527873607835451ull -> 1109289945107352897ull
//   4735701892803300920ull -> 15892759426148401507ull
//   8063401603116370931ull -> 17112119348245367605ull
//   18294135471370528909ull -> 8581843276479262047ull
//   16661456640802028241ull -> 7131344654937855853ull
//   14772433093186204495ull -> 14792600582710493575ull
//   7620644898991603805ull -> 1239841573570926230ull
//   15118496304273749831ull -> 5444729147284661655ull
//   7230774509547727832ull -> 3245433697010211721ull
//   9367416904717233797ull -> 4676939050322725348ull
//   29939079855673758ull -> 7350099411866253542ull
//   17645391567178045076ull -> 343377133147017054ull
//   4152792192432616558ull -> 9598996651795533367ull
//   1290992315178131787ull -> 14030542661451411676ull
//   11629813004726792644ull -> 9219466268922236720ull
//   14092163688413375773ull -> 6348273631916125388ull
//   2250285108640531229ull -> 1051431539591183352ull
//   8323804787620377009ull -> 2350462348068176311ull
//   12084061299001209834ull -> 5268079867744237915ull
//   6575350347839890721ull -> 2325247658527992044ull
//   8511425819615280004ull -> 6174548207444471958ull
//   3716074811611924003ull -> 8596767173582221714ull
//   17462716235150650529ull -> 986987187009250324ull
//   14424451358928265509ull -> 13994596646214270165ull
//   8812190484420866093ull -> 2602688672529104322ull
//   8378584172590769377ull -> 3911601850678352661ull
//   12342327263055300482ull -> 9593288478406285213ull
//   5755733331066026585ull -> 4236346333119319709ull
//   11985055179349118870ull -> 15363079072004362933ull
//   1729850769323221596ull -> 18107900300087896759ull
//   8374018043578414321ull -> 17921584433192374312ull
//   10405234474490090389ull -> 5816305515259473479ull
//   18200276816877882479ull -> 10970209394144352158ull
//   6678532068539452832ull -> 7950770882156242927ull
//   7638978804204415889ull -> 18330351249071377885ull
//   1592570382804466430ull -> 13975661199209811620ull
//   4258395109503891217ull -> 11989522059105970117ull
//   14202785402043642137ull -> 14967623866548664000ull
//   12763565106297155564ull -> 13208968445305919179ull
// expectation corrected (V19-FIX, k_recording, 71 of 72 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   2543779130970217232ull -> 14621553252499744072ull
//   13638291719838344805ull -> 16947653718640119089ull
//   17904934979399943420ull -> 8181232920474966978ull
//   4916994404502736826ull -> 384032772146433484ull
//   13208968445305919179ull -> 4370795498617211977ull
//   10324478866479362969ull -> 10361542891827113526ull
//   13208968445305919179ull -> 4370795498617211977ull
//   10364728431969872992ull -> 11635388043626139808ull
//   6773723984513471634ull -> 5137642655964282706ull
//   17279077540384140970ull -> 1067158101705749258ull
//   273555295542222474ull -> 17589216349291364682ull
//   8791634334263737749ull -> 13103826456263923669ull
//   925668679100948855ull -> 14125475791418217750ull
//   8777619534496248594ull -> 5496734832958426517ull
//   17725129497555317017ull -> 3300864403891436803ull
//   8008837844251753449ull -> 14334667028075329258ull
//   5594741258697960485ull -> 19027534220486873ull
//   15079181317456407482ull -> 17400684420983527587ull
//   17089349116617098209ull -> 16843748232757858737ull
//   18200208521056000396ull -> 5910420545552211065ull
//   15241045489297703683ull -> 4037494890945270795ull
//   1762199573452402758ull -> 13685270228904493022ull
//   11385708691232883797ull -> 12610051923577992823ull
//   17964225683996893310ull -> 13419685112760992640ull
//   4534429732864628476ull -> 6561776234336247499ull
//   17467596193696644527ull -> 11286535916465691925ull
//   16401650137151877883ull -> 9286401377316929631ull
//   16808577086136594190ull -> 11941619648187175172ull
//   12964209703146465173ull -> 2781110012097249044ull
//   16463685165528796935ull -> 5152088055254729287ull
//   17915487794021341896ull -> 14925422330047382771ull
//   15978106025216492785ull -> 18068979986855895194ull
//   1109289945107352897ull -> 3293728938989802792ull
//   15892759426148401507ull -> 3040154494312885091ull
//   17112119348245367605ull -> 3914527074597397115ull
//   8581843276479262047ull -> 6028800054409062637ull
//   7131344654937855853ull -> 17539544361470737737ull
//   14792600582710493575ull -> 17497902664474484401ull
//   1239841573570926230ull -> 16821147754446235025ull
//   5444729147284661655ull -> 3501965129295230921ull
//   3245433697010211721ull -> 17136543747664596622ull
//   4676939050322725348ull -> 6796102544471338071ull
//   7350099411866253542ull -> 10258488113034267824ull
//   343377133147017054ull -> 15437545425183691961ull
//   9598996651795533367ull -> 10208872586329130020ull
//   14030542661451411676ull -> 14368668855499836453ull
//   9219466268922236720ull -> 628171839165946395ull
//   6348273631916125388ull -> 10702221356985455012ull
//   1051431539591183352ull -> 2314157047087909764ull
//   2350462348068176311ull -> 12920471494293590838ull
//   5268079867744237915ull -> 8752737450779041921ull
//   2325247658527992044ull -> 7994102271719463356ull
//   6174548207444471958ull -> 4741946402210887143ull
//   8596767173582221714ull -> 739366574823717504ull
//   986987187009250324ull -> 3655914840365203865ull
//   13994596646214270165ull -> 7448010022416009296ull
//   2602688672529104322ull -> 11266041923012349372ull
//   3911601850678352661ull -> 16061447354858643806ull
//   9593288478406285213ull -> 6641998937765131086ull
//   4236346333119319709ull -> 15745847663605277053ull
//   15363079072004362933ull -> 2232785799096296677ull
//   18107900300087896759ull -> 12057790076704791676ull
//   17921584433192374312ull -> 14126847163984952264ull
//   5816305515259473479ull -> 6703125855558665936ull
//   10970209394144352158ull -> 750370809058189391ull
//   7950770882156242927ull -> 727242285392145435ull
//   18330351249071377885ull -> 9029503590661437192ull
//   13975661199209811620ull -> 6205861443650071106ull
//   11989522059105970117ull -> 5548300568428407565ull
//   14967623866548664000ull -> 4191735526950747220ull
//   13208968445305919179ull -> 4370795498617211977ull
// expectation corrected (INT25, k_recording, 71 of 72 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   14621553252499744072ull -> 5575980828577033360ull
//   16947653718640119089ull -> 16309388099732889217ull
//   8181232920474966978ull -> 16463834114134690168ull
//   384032772146433484ull -> 14697449118452563264ull
//   4370795498617211977ull -> 2898399952537902865ull
//   10361542891827113526ull -> 8964175343669296125ull
//   4370795498617211977ull -> 2898399952537902865ull
//   11635388043626139808ull -> 11912929120179152844ull
//   5137642655964282706ull -> 7111499071038382551ull
//   1067158101705749258ull -> 14395892391241930212ull
//   17589216349291364682ull -> 9034635987304089269ull
//   13103826456263923669ull -> 7745035282290493167ull
//   14125475791418217750ull -> 9653978480792136139ull
//   5496734832958426517ull -> 6575784925310671623ull
//   3300864403891436803ull -> 16756306850270906652ull
//   14334667028075329258ull -> 6791844355462743637ull
//   19027534220486873ull -> 9752175874850418240ull
//   17400684420983527587ull -> 5581794927458177019ull
//   16843748232757858737ull -> 16367531041072987671ull
//   5910420545552211065ull -> 5499206076623092635ull
//   4037494890945270795ull -> 16891823664600062531ull
//   13685270228904493022ull -> 15306266440153708710ull
//   12610051923577992823ull -> 8382791724770590634ull
//   13419685112760992640ull -> 4926336483410460532ull
//   6561776234336247499ull -> 12823090459845782834ull
//   11286535916465691925ull -> 10217177223946990874ull
//   9286401377316929631ull -> 12680312482263936655ull
//   11941619648187175172ull -> 15032085204869760060ull
//   2781110012097249044ull -> 7696336452231268012ull
//   5152088055254729287ull -> 6819477884530924764ull
//   14925422330047382771ull -> 13932522791731114426ull
//   18068979986855895194ull -> 15842817164955438950ull
//   3293728938989802792ull -> 4250764768037495411ull
//   3040154494312885091ull -> 1760469320874157114ull
//   3914527074597397115ull -> 7565330933885892856ull
//   6028800054409062637ull -> 3348650657119335616ull
//   17539544361470737737ull -> 16026642042438818681ull
//   17497902664474484401ull -> 15336121108426360137ull
//   16821147754446235025ull -> 12610087835245307199ull
//   3501965129295230921ull -> 3169655936610324835ull
//   17136543747664596622ull -> 18005345939863936504ull
//   6796102544471338071ull -> 10374887129263366256ull
//   10258488113034267824ull -> 3316605285128650350ull
//   15437545425183691961ull -> 10411230090089985874ull
//   10208872586329130020ull -> 2321647345879300563ull
//   14368668855499836453ull -> 9282855342318040804ull
//   628171839165946395ull -> 8782939554936041465ull
//   10702221356985455012ull -> 9270809468617755726ull
//   2314157047087909764ull -> 9705564861311910669ull
//   12920471494293590838ull -> 1984027035687621850ull
//   8752737450779041921ull -> 5309816640440296119ull
//   7994102271719463356ull -> 61906512444861614ull
//   4741946402210887143ull -> 18408253739502061131ull
//   739366574823717504ull -> 2900636347809437670ull
//   3655914840365203865ull -> 8590510826456084479ull
//   7448010022416009296ull -> 16739036358771975930ull
//   11266041923012349372ull -> 3846505090147632664ull
//   16061447354858643806ull -> 13346518438036570112ull
//   6641998937765131086ull -> 3939181335635768762ull
//   15745847663605277053ull -> 1996675722330704212ull
//   2232785799096296677ull -> 271113896427528122ull
//   12057790076704791676ull -> 1511893874670543988ull
//   14126847163984952264ull -> 15973548105797383183ull
//   6703125855558665936ull -> 4392899708297163730ull
//   750370809058189391ull -> 11467883812317724901ull
//   727242285392145435ull -> 11051413718335140252ull
//   9029503590661437192ull -> 903533060614176790ull
//   6205861443650071106ull -> 14466209299323094098ull
//   5548300568428407565ull -> 4010960734010157804ull
//   4191735526950747220ull -> 5172512134487820157ull
//   4370795498617211977ull -> 2898399952537902865ull
// expectation corrected (INT26 v19 hash re-pin, k_recording, 68 of 72 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   16309388099732889217ull -> 13909724611150684752ull [H-THIN]
//   14697449118452563264ull -> 6058006111773907271ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
//   11912929120179152844ull -> 14971695095207148331ull [H-THIN]
//   7111499071038382551ull -> 10535748930949530126ull [H-THIN]
//   14395892391241930212ull -> 12991008539092747157ull [H-THIN]
//   9034635987304089269ull -> 15956762843095239782ull [H-THIN]
//   7745035282290493167ull -> 2607245150229644476ull [H-THIN]
//   9653978480792136139ull -> 2184445209065093070ull [H-THIN]
//   6575784925310671623ull -> 2897664079743275822ull [H-THIN]
//   16756306850270906652ull -> 4157996150621490761ull [H-THIN]
//   6791844355462743637ull -> 10537503143714109070ull [H-THIN]
//   9752175874850418240ull -> 13754546407945448507ull [H-THIN]
//   5581794927458177019ull -> 16440431375654315850ull [H-THIN]
//   16367531041072987671ull -> 7003661191077414576ull [H-THIN]
//   5499206076623092635ull -> 11353048097065184288ull [H-THIN]
//   16891823664600062531ull -> 12848736740973846400ull [H-THIN]
//   15306266440153708710ull -> 3161842879106046797ull [H-THIN]
//   8382791724770590634ull -> 10847056918133263985ull [H-THIN]
//   4926336483410460532ull -> 8350647617751486789ull [H-THIN]
//   12823090459845782834ull -> 1856794670313281419ull [H-THIN]
//   10217177223946990874ull -> 3189033237966511301ull [H-THIN]
//   12680312482263936655ull -> 9556919139953515754ull [H-THIN]
//   15032085204869760060ull -> 5681429441061062241ull [H-THIN]
//   7696336452231268012ull -> 10425853480784439841ull [H-THIN]
//   6819477884530924764ull -> 12097190747565164239ull [H-THIN]
//   13932522791731114426ull -> 12637836836690018377ull [H-THIN]
//   15842817164955438950ull -> 14424918618138801803ull [H-THIN]
//   4250764768037495411ull -> 17903030095845904958ull [H-THIN]
//   1760469320874157114ull -> 15510962773836543503ull [H-THIN]
//   7565330933885892856ull -> 16031650111081263649ull [H-THIN]
//   3348650657119335616ull -> 15453192344610479715ull [H-THIN]
//   16026642042438818681ull -> 12604736624722151438ull [H-THIN]
//   15336121108426360137ull -> 17308957480242505390ull [H-THIN]
//   12610087835245307199ull -> 452142121530738962ull [H-THIN]
//   3169655936610324835ull -> 6150193546982553982ull [H-THIN]
//   18005345939863936504ull -> 12917335185226580263ull [H-THIN]
//   10374887129263366256ull -> 3249697982624166735ull [H-THIN]
//   3316605285128650350ull -> 3343978410969230181ull [H-THIN]
//   10411230090089985874ull -> 16247831777829745607ull [H-THIN]
//   2321647345879300563ull -> 7514289197254818298ull [H-THIN]
//   9282855342318040804ull -> 17972948283988009341ull [H-THIN]
//   8782939554936041465ull -> 4872902272073809254ull [H-THIN]
//   9270809468617755726ull -> 16118270799549857939ull [H-THIN]
//   9705564861311910669ull -> 14672005653726242528ull [H-THIN]
//   1984027035687621850ull -> 2133921838620278979ull [H-THIN]
//   5309816640440296119ull -> 13677532362142245502ull [H-THIN]
//   61906512444861614ull -> 7883065542401673469ull [H-THIN]
//   18408253739502061131ull -> 7126839422434123000ull [H-THIN]
//   2900636347809437670ull -> 3907128035298066349ull [H-THIN]
//   8590510826456084479ull -> 4948527793100784708ull [H-THIN]
//   16739036358771975930ull -> 142644456983149613ull [H-THIN]
//   3846505090147632664ull -> 14555917326272639085ull [H-THIN]
//   13346518438036570112ull -> 5212018862612370299ull [H-THIN]
//   3939181335635768762ull -> 865072719653853777ull [H-THIN]
//   1996675722330704212ull -> 14621783374817857573ull [H-THIN]
//   271113896427528122ull -> 12663926541947072239ull [H-THIN]
//   1511893874670543988ull -> 7631475035369278017ull [H-THIN]
//   15973548105797383183ull -> 951171557373679674ull [H-THIN]
//   4392899708297163730ull -> 9891512222295021637ull [H-THIN]
//   11467883812317724901ull -> 14067535954397498792ull [H-THIN]
//   11051413718335140252ull -> 14311549026741467081ull [H-THIN]
//   903533060614176790ull -> 775415503723595045ull [H-THIN]
//   14466209299323094098ull -> 2025287021890762573ull [H-THIN]
//   4010960734010157804ull -> 5576052922734185379ull [H-THIN]
//   5172512134487820157ull -> 1575805364117614574ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
constexpr std::uint64_t k_recording[] = {
    5575980828577033360ull,
    13909724611150684752ull,
    16463834114134690168ull,
    6058006111773907271ull,
    13929697350787785222ull,
    8964175343669296125ull,
    13929697350787785222ull,
    14971695095207148331ull,
    10535748930949530126ull,
    12991008539092747157ull,
    15956762843095239782ull,
    2607245150229644476ull,
    2184445209065093070ull,
    2897664079743275822ull,
    4157996150621490761ull,
    10537503143714109070ull,
    13754546407945448507ull,
    16440431375654315850ull,
    7003661191077414576ull,
    11353048097065184288ull,
    12848736740973846400ull,
    3161842879106046797ull,
    10847056918133263985ull,
    8350647617751486789ull,
    1856794670313281419ull,
    3189033237966511301ull,
    9556919139953515754ull,
    5681429441061062241ull,
    10425853480784439841ull,
    12097190747565164239ull,
    12637836836690018377ull,
    14424918618138801803ull,
    17903030095845904958ull,
    15510962773836543503ull,
    16031650111081263649ull,
    15453192344610479715ull,
    12604736624722151438ull,
    17308957480242505390ull,
    452142121530738962ull,
    6150193546982553982ull,
    12917335185226580263ull,
    3249697982624166735ull,
    3343978410969230181ull,
    16247831777829745607ull,
    7514289197254818298ull,
    17972948283988009341ull,
    4872902272073809254ull,
    16118270799549857939ull,
    14672005653726242528ull,
    2133921838620278979ull,
    13677532362142245502ull,
    7883065542401673469ull,
    7126839422434123000ull,
    3907128035298066349ull,
    4948527793100784708ull,
    142644456983149613ull,
    14555917326272639085ull,
    5212018862612370299ull,
    865072719653853777ull,
    14621783374817857573ull,
    12663926541947072239ull,
    7631475035369278017ull,
    951171557373679674ull,
    9891512222295021637ull,
    14067535954397498792ull,
    14311549026741467081ull,
    775415503723595045ull,
    2025287021890762573ull,
    5576052922734185379ull,
    1575805364117614574ull,
    13929697350787785222ull,
    8ull,
};
// expectation corrected (k_stream, 54 of 54 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   1675429297538291515ull -> 9926360377267477400ull
//   4269294312618103906ull -> 6983879323613695038ull
//   2739383661395394878ull -> 15722806537016766703ull
//   8562514945273635583ull -> 3639719031233734366ull
//   4615219708780803716ull -> 6715353660937589514ull
//   18330196230098296390ull -> 6443204691850407923ull
//   3554632640474636312ull -> 1658944240032470522ull
//   10417047650818886325ull -> 4564528776249873855ull
//   650739273241778796ull -> 17316063992144767815ull
//   12108855568513478307ull -> 1138673537377377155ull
//   8711454243523497982ull -> 4984525601846280238ull
//   2223332407915328625ull -> 8694866053676819856ull
//   1217751541106315384ull -> 1584577151820503907ull
//   16471588522613747832ull -> 7336426762353074103ull
//   6736841173273024464ull -> 4297651875135187523ull
//   1087048003738547363ull -> 3358546249625000219ull
//   16588021202856837411ull -> 9774203748618497442ull
//   17253755521927023300ull -> 15023261739455876001ull
//   1243981779487650256ull -> 17982483925466052345ull
//   14900358392540784930ull -> 12067274609566293803ull
//   6778286763750019994ull -> 2795487836124372222ull
//   14481333550362977211ull -> 16924141763831892887ull
//   10121209301347601047ull -> 10758346952141907762ull
//   5360742753374224395ull -> 2311051631285001948ull
//   7194604531456821769ull -> 16941051314076490383ull
//   6338469559424142063ull -> 8023409289033289423ull
//   14448145086843768389ull -> 10902677858936058066ull
//   6126764331570430362ull -> 13443126914813445573ull
//   2429900482415992307ull -> 5715540528381471121ull
//   12230285780283388586ull -> 11598049666469522901ull
//   13342321052017506483ull -> 6439204370199141165ull
//   15249759142581094789ull -> 4888547293499339163ull
//   14900083559242955391ull -> 6866984268264696985ull
//   9767880500898670645ull -> 3759587071855175591ull
//   8448006643156941385ull -> 10133115201077270549ull
//   14539435280093932696ull -> 18414563990090124433ull
//   12864892018768837699ull -> 788358065976043576ull
//   11829225551267815714ull -> 632402651546170885ull
//   5746539048806255200ull -> 10146430786139564947ull
//   15796989791232018348ull -> 16999736537802937080ull
//   4781341795856248313ull -> 6008469009194404982ull
//   15874036727099874755ull -> 1504430507355741288ull
//   11893920550216247558ull -> 11660732259755016903ull
//   9836983884308946468ull -> 15629776065187965767ull
//   412394026712311842ull -> 561208533436643181ull
//   8542491185870079050ull -> 4727655756242265723ull
//   10735459024569195401ull -> 1093697520232017686ull
//   12663209468020544370ull -> 14400352229660723061ull
//   14516542216854479084ull -> 18171012142792015032ull
//   461594562865019724ull -> 15414161288850079440ull
//   10273329865594665922ull -> 5839767826071636333ull
//   461594562865019724ull -> 15414161288850079440ull
//   3105017244239933332ull -> 4057136666456172998ull
//   461594562865019724ull -> 15414161288850079440ull
// expectation corrected (v19-B, k_stream, 54 of 54 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   9926360377267477400ull -> 948783147963005660ull
//   6983879323613695038ull -> 6258262929575333087ull
//   15722806537016766703ull -> 17513437510661292093ull
//   3639719031233734366ull -> 14992337489397360393ull
//   6715353660937589514ull -> 4371503310698045362ull
//   6443204691850407923ull -> 7634998654808690043ull
//   1658944240032470522ull -> 3845678135899710043ull
//   4564528776249873855ull -> 60626724102493512ull
//   17316063992144767815ull -> 16503481641668561908ull
//   1138673537377377155ull -> 15040106242869486882ull
//   4984525601846280238ull -> 1213586276935313252ull
//   8694866053676819856ull -> 7105826286621370478ull
//   1584577151820503907ull -> 13080487245400062203ull
//   7336426762353074103ull -> 9900065397881108365ull
//   4297651875135187523ull -> 11175928055869418645ull
//   3358546249625000219ull -> 15196193893632458680ull
//   9774203748618497442ull -> 12574930759015574656ull
//   15023261739455876001ull -> 180229384902392595ull
//   17982483925466052345ull -> 14931811156527589396ull
//   12067274609566293803ull -> 7025769596000376474ull
//   2795487836124372222ull -> 11128902767468582530ull
//   16924141763831892887ull -> 17287768040640926785ull
//   10758346952141907762ull -> 8046156127041051839ull
//   2311051631285001948ull -> 2576933522647501972ull
//   16941051314076490383ull -> 11516981055053986601ull
//   8023409289033289423ull -> 15040561571098226314ull
//   10902677858936058066ull -> 5215032498854786682ull
//   13443126914813445573ull -> 15426973044609261854ull
//   5715540528381471121ull -> 6732542386604522331ull
//   11598049666469522901ull -> 3501344427354627919ull
//   6439204370199141165ull -> 17375367742411431301ull
//   4888547293499339163ull -> 1739880003141880127ull
//   6866984268264696985ull -> 2467030153255185580ull
//   3759587071855175591ull -> 16193138952966854474ull
//   10133115201077270549ull -> 14145180498168891911ull
//   18414563990090124433ull -> 14256374645744598116ull
//   788358065976043576ull -> 1560558501180967363ull
//   632402651546170885ull -> 1625887836699742727ull
//   10146430786139564947ull -> 5084138944843925215ull
//   16999736537802937080ull -> 16049889893817296759ull
//   6008469009194404982ull -> 6763730610750202545ull
//   1504430507355741288ull -> 7302244181711400375ull
//   11660732259755016903ull -> 18123708192449447342ull
//   15629776065187965767ull -> 9301931766718459449ull
//   561208533436643181ull -> 15571025647897925417ull
//   4727655756242265723ull -> 4543349431016275683ull
//   1093697520232017686ull -> 5309579489344409296ull
//   14400352229660723061ull -> 8537547155714496862ull
//   18171012142792015032ull -> 13163296717902447249ull
//   15414161288850079440ull -> 14751690121258578908ull
//   5839767826071636333ull -> 5882164565997988658ull
//   15414161288850079440ull -> 14751690121258578908ull
//   4057136666456172998ull -> 11033218740417797382ull
//   15414161288850079440ull -> 14751690121258578908ull
// expectation corrected (v19-E, k_stream, 39 of 54 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   948783147963005660ull -> 7287402210134811723ull
//   17513437510661292093ull -> 3613812001802944490ull
//   14992337489397360393ull -> 14256716415753092846ull
//   4371503310698045362ull -> 18066710111273402560ull
//   7634998654808690043ull -> 14450906895180287465ull
//   60626724102493512ull -> 7810034545682398155ull
//   16503481641668561908ull -> 15431396809014267622ull
//   1213586276935313252ull -> 15078256075606303379ull
//   13080487245400062203ull -> 807892153055512972ull
//   9900065397881108365ull -> 8496224644929899065ull
//   11175928055869418645ull -> 17064616757066975641ull
//   12574930759015574656ull -> 7052726630314129836ull
//   180229384902392595ull -> 8205504961151388183ull
//   14931811156527589396ull -> 1165523721440006008ull
//   7025769596000376474ull -> 10509778316010069121ull
//   17287768040640926785ull -> 12820182186070573599ull
//   8046156127041051839ull -> 1003285434194249605ull
//   11516981055053986601ull -> 9459669056269242935ull
//   5215032498854786682ull -> 16298324343950560662ull
//   15426973044609261854ull -> 7295767240173679712ull
//   6732542386604522331ull -> 11303232892540595619ull
//   3501344427354627919ull -> 9108531251772622477ull
//   1739880003141880127ull -> 13980272045865766065ull
//   2467030153255185580ull -> 7431106375660941195ull
//   16193138952966854474ull -> 8791307287609743160ull
//   14256374645744598116ull -> 11868273362524112458ull
//   1560558501180967363ull -> 9898567439601994650ull
//   1625887836699742727ull -> 2093231171095230746ull
//   16049889893817296759ull -> 15691180810974081764ull
//   6763730610750202545ull -> 4498955898635597792ull
//   7302244181711400375ull -> 16786848328132643741ull
//   9301931766718459449ull -> 4395122555036264306ull
//   15571025647897925417ull -> 10159557292487317173ull
//   4543349431016275683ull -> 12231697623938131977ull
//   8537547155714496862ull -> 8546285193301548912ull
//   13163296717902447249ull -> 6138899047874310618ull
//   14751690121258578908ull -> 10443374725851587239ull
//   14751690121258578908ull -> 10443374725851587239ull
//   14751690121258578908ull -> 10443374725851587239ull
// expectation corrected (V19-FIX, k_stream, 54 of 54 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   7287402210134811723ull -> 16510252855932086823ull
//   6258262929575333087ull -> 14048324983519873828ull
//   3613812001802944490ull -> 17804079663198632649ull
//   14256716415753092846ull -> 7665081442761407167ull
//   18066710111273402560ull -> 1510562666313522433ull
//   14450906895180287465ull -> 16253369998080909938ull
//   3845678135899710043ull -> 15976513930905472129ull
//   7810034545682398155ull -> 3846751227738950664ull
//   15431396809014267622ull -> 15450124388103584652ull
//   15040106242869486882ull -> 2225665425051551577ull
//   15078256075606303379ull -> 15615312821753524918ull
//   7105826286621370478ull -> 11958084298410326056ull
//   807892153055512972ull -> 7978187604899367877ull
//   8496224644929899065ull -> 876738446875159256ull
//   17064616757066975641ull -> 3498362567022607364ull
//   15196193893632458680ull -> 7053403205296459880ull
//   7052726630314129836ull -> 11858356529053237ull
//   8205504961151388183ull -> 5137924053122990913ull
//   1165523721440006008ull -> 1455779883697863925ull
//   10509778316010069121ull -> 1450972605221627516ull
//   11128902767468582530ull -> 6748150359618158603ull
//   12820182186070573599ull -> 17100694157255179539ull
//   1003285434194249605ull -> 16383805058165184155ull
//   2576933522647501972ull -> 14458762612387161134ull
//   9459669056269242935ull -> 7578874640041129857ull
//   15040561571098226314ull -> 1091721997205765161ull
//   16298324343950560662ull -> 4972829120680344696ull
//   7295767240173679712ull -> 11905106519757767589ull
//   11303232892540595619ull -> 14990642672847258155ull
//   9108531251772622477ull -> 18051484560037243770ull
//   17375367742411431301ull -> 7846354589731725875ull
//   13980272045865766065ull -> 1584652476433904101ull
//   7431106375660941195ull -> 12788838416146221578ull
//   8791307287609743160ull -> 859043901073302035ull
//   14145180498168891911ull -> 5668633829937549152ull
//   11868273362524112458ull -> 7252967951263295973ull
//   9898567439601994650ull -> 13226339972060618816ull
//   2093231171095230746ull -> 1981332788226950674ull
//   5084138944843925215ull -> 17570152268719247518ull
//   15691180810974081764ull -> 808203791867106338ull
//   4498955898635597792ull -> 8732491230892848194ull
//   16786848328132643741ull -> 960688443443831230ull
//   18123708192449447342ull -> 8967860587877234272ull
//   4395122555036264306ull -> 12765059320032069741ull
//   10159557292487317173ull -> 3138802922902822053ull
//   12231697623938131977ull -> 6653748162035956560ull
//   5309579489344409296ull -> 12107272552955766436ull
//   8546285193301548912ull -> 11629978161529728893ull
//   6138899047874310618ull -> 12532033173457401429ull
//   10443374725851587239ull -> 8444087394523764362ull
//   5882164565997988658ull -> 13899343461558324542ull
//   10443374725851587239ull -> 8444087394523764362ull
//   11033218740417797382ull -> 14079671624423886806ull
//   10443374725851587239ull -> 8444087394523764362ull
// expectation corrected (INT25, k_stream, 54 of 54 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   16510252855932086823ull -> 16458853630937199693ull
//   14048324983519873828ull -> 10534014544629119321ull
//   17804079663198632649ull -> 15852567477617018112ull
//   7665081442761407167ull -> 8395254103899656747ull
//   1510562666313522433ull -> 9137153746257222586ull
//   16253369998080909938ull -> 4321243615010347791ull
//   15976513930905472129ull -> 2464381025353933752ull
//   3846751227738950664ull -> 4335032133758631355ull
//   15450124388103584652ull -> 3418642895666580588ull
//   2225665425051551577ull -> 12043850726481513634ull
//   15615312821753524918ull -> 15778603109418817912ull
//   11958084298410326056ull -> 7422979898823356625ull
//   7978187604899367877ull -> 8082887755714141501ull
//   876738446875159256ull -> 18272796975887582060ull
//   3498362567022607364ull -> 4620349392656757831ull
//   7053403205296459880ull -> 11062470475981718812ull
//   11858356529053237ull -> 11681022048666484124ull
//   5137924053122990913ull -> 1110410394930231379ull
//   1455779883697863925ull -> 8353289200898942510ull
//   1450972605221627516ull -> 5464661787014092283ull
//   6748150359618158603ull -> 18042314908697358845ull
//   17100694157255179539ull -> 17913569146247237990ull
//   16383805058165184155ull -> 14252084119842247317ull
//   14458762612387161134ull -> 13951402111806527659ull
//   7578874640041129857ull -> 14984524306684102418ull
//   1091721997205765161ull -> 4929388494852177725ull
//   4972829120680344696ull -> 8427843384471180011ull
//   11905106519757767589ull -> 12312569890916686665ull
//   14990642672847258155ull -> 7806471840435792838ull
//   18051484560037243770ull -> 10841176947711978698ull
//   7846354589731725875ull -> 14141658027994245481ull
//   1584652476433904101ull -> 15796138674937755121ull
//   12788838416146221578ull -> 4930499034900633496ull
//   859043901073302035ull -> 11894749826123257324ull
//   5668633829937549152ull -> 331885767850924737ull
//   7252967951263295973ull -> 6209022505833017115ull
//   13226339972060618816ull -> 13004202639896233962ull
//   1981332788226950674ull -> 1029059189446661931ull
//   17570152268719247518ull -> 9179914352160302837ull
//   808203791867106338ull -> 9236479431486641300ull
//   8732491230892848194ull -> 5209128789544115679ull
//   960688443443831230ull -> 5582126543454279956ull
//   8967860587877234272ull -> 14370442711223214011ull
//   12765059320032069741ull -> 16435999169585034112ull
//   3138802922902822053ull -> 2858404675926398753ull
//   6653748162035956560ull -> 2863207613356019619ull
//   12107272552955766436ull -> 4197841241754323017ull
//   11629978161529728893ull -> 1651019462700250297ull
//   12532033173457401429ull -> 9342015881796079139ull
//   8444087394523764362ull -> 971242319250035563ull
//   13899343461558324542ull -> 6367780246848246992ull
//   8444087394523764362ull -> 971242319250035563ull
//   14079671624423886806ull -> 6931684686417541656ull
//   8444087394523764362ull -> 971242319250035563ull
// expectation corrected (INT26 v19 hash re-pin, k_stream, 39 of 54 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   16458853630937199693ull -> 6675987432147888000ull [H-THIN]
//   15852567477617018112ull -> 15316836834528354089ull [H-THIN]
//   8395254103899656747ull -> 122347678355491576ull [H-THIN]
//   9137153746257222586ull -> 17060189170921966279ull [H-THIN]
//   4321243615010347791ull -> 12998233506120003610ull [H-THIN]
//   4335032133758631355ull -> 5790160451626852554ull [H-THIN]
//   3418642895666580588ull -> 8747488118396803171ull [H-THIN]
//   15778603109418817912ull -> 11301555486053984887ull [H-THIN]
//   8082887755714141501ull -> 16019190197976852534ull [H-THIN]
//   18272796975887582060ull -> 14554266247005678391ull [H-THIN]
//   4620349392656757831ull -> 9863308761378043570ull [H-THIN]
//   11681022048666484124ull -> 3981212442403576087ull [H-THIN]
//   1110410394930231379ull -> 16893712934576968012ull [H-THIN]
//   8353289200898942510ull -> 17103134812120181635ull [H-THIN]
//   5464661787014092283ull -> 2716679541586828074ull [H-THIN]
//   17913569146247237990ull -> 2142815117417671925ull [H-THIN]
//   14252084119842247317ull -> 1297101144475427900ull [H-THIN]
//   14984524306684102418ull -> 17860190683404402179ull [H-THIN]
//   8427843384471180011ull -> 12743930332679229528ull [H-THIN]
//   12312569890916686665ull -> 13672064593010625780ull [H-THIN]
//   7806471840435792838ull -> 15322331001617686957ull [H-THIN]
//   10841176947711978698ull -> 2540452936526860029ull [H-THIN]
//   15796138674937755121ull -> 5707160760001918266ull [H-THIN]
//   4930499034900633496ull -> 17256519628932910437ull [H-THIN]
//   11894749826123257324ull -> 1265742856577179629ull [H-THIN]
//   6209022505833017115ull -> 8935675646451046114ull [H-THIN]
//   13004202639896233962ull -> 1995113508368758049ull [H-THIN]
//   1029059189446661931ull -> 12126669220334860080ull [H-THIN]
//   9236479431486641300ull -> 13100650686759242355ull [H-THIN]
//   5209128789544115679ull -> 1320523439935521014ull [H-THIN]
//   5582126543454279956ull -> 10835572324283436971ull [H-THIN]
//   16435999169585034112ull -> 411402308123821837ull [H-THIN]
//   2858404675926398753ull -> 18325644631502656152ull [H-THIN]
//   2863207613356019619ull -> 6533533400826708814ull [H-THIN]
//   1651019462700250297ull -> 11035145778810736216ull [H-THIN]
//   9342015881796079139ull -> 4664106270365911266ull [H-THIN]
//   971242319250035563ull -> 6512545530849110454ull [H-THIN]
//   971242319250035563ull -> 6512545530849110454ull [H-THIN]
//   971242319250035563ull -> 6512545530849110454ull [H-THIN]
constexpr std::uint64_t k_stream[] = {
    6675987432147888000ull,
    10534014544629119321ull,
    15316836834528354089ull,
    122347678355491576ull,
    17060189170921966279ull,
    12998233506120003610ull,
    2464381025353933752ull,
    5790160451626852554ull,
    8747488118396803171ull,
    12043850726481513634ull,
    11301555486053984887ull,
    7422979898823356625ull,
    16019190197976852534ull,
    14554266247005678391ull,
    9863308761378043570ull,
    11062470475981718812ull,
    3981212442403576087ull,
    16893712934576968012ull,
    17103134812120181635ull,
    2716679541586828074ull,
    18042314908697358845ull,
    2142815117417671925ull,
    1297101144475427900ull,
    13951402111806527659ull,
    17860190683404402179ull,
    4929388494852177725ull,
    12743930332679229528ull,
    13672064593010625780ull,
    15322331001617686957ull,
    2540452936526860029ull,
    14141658027994245481ull,
    5707160760001918266ull,
    17256519628932910437ull,
    1265742856577179629ull,
    331885767850924737ull,
    8935675646451046114ull,
    1995113508368758049ull,
    12126669220334860080ull,
    9179914352160302837ull,
    13100650686759242355ull,
    1320523439935521014ull,
    10835572324283436971ull,
    14370442711223214011ull,
    411402308123821837ull,
    18325644631502656152ull,
    6533533400826708814ull,
    4197841241754323017ull,
    11035145778810736216ull,
    4664106270365911266ull,
    6512545530849110454ull,
    6367780246848246992ull,
    6512545530849110454ull,
    6931684686417541656ull,
    6512545530849110454ull,
};
// expectation corrected (k_stream_aborted, 35 of 35 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   1675429297538291515ull -> 9926360377267477400ull
//   4269294312618103906ull -> 6983879323613695038ull
//   2739383661395394878ull -> 15722806537016766703ull
//   8562514945273635583ull -> 3639719031233734366ull
//   4615219708780803716ull -> 6715353660937589514ull
//   18330196230098296390ull -> 6443204691850407923ull
//   3554632640474636312ull -> 1658944240032470522ull
//   10417047650818886325ull -> 4564528776249873855ull
//   650739273241778796ull -> 17316063992144767815ull
//   12108855568513478307ull -> 1138673537377377155ull
//   8711454243523497982ull -> 4984525601846280238ull
//   2223332407915328625ull -> 8694866053676819856ull
//   1217751541106315384ull -> 1584577151820503907ull
//   16471588522613747832ull -> 7336426762353074103ull
//   6736841173273024464ull -> 4297651875135187523ull
//   1087048003738547363ull -> 3358546249625000219ull
//   16588021202856837411ull -> 9774203748618497442ull
//   17253755521927023300ull -> 15023261739455876001ull
//   1243981779487650256ull -> 17982483925466052345ull
//   14900358392540784930ull -> 12067274609566293803ull
//   6778286763750019994ull -> 2795487836124372222ull
//   14481333550362977211ull -> 16924141763831892887ull
//   10121209301347601047ull -> 10758346952141907762ull
//   5360742753374224395ull -> 2311051631285001948ull
//   7194604531456821769ull -> 16941051314076490383ull
//   6338469559424142063ull -> 8023409289033289423ull
//   14448145086843768389ull -> 10902677858936058066ull
//   6126764331570430362ull -> 13443126914813445573ull
//   2429900482415992307ull -> 5715540528381471121ull
//   12230285780283388586ull -> 11598049666469522901ull
//   13342321052017506483ull -> 6439204370199141165ull
//   15249759142581094789ull -> 4888547293499339163ull
//   14900083559242955391ull -> 6866984268264696985ull
//   6779992811708797118ull -> 7480253846593316468ull
//   14900083559242955391ull -> 6866984268264696985ull
// expectation corrected (v19-B, k_stream_aborted, 35 of 35 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   9926360377267477400ull -> 948783147963005660ull
//   6983879323613695038ull -> 6258262929575333087ull
//   15722806537016766703ull -> 17513437510661292093ull
//   3639719031233734366ull -> 14992337489397360393ull
//   6715353660937589514ull -> 4371503310698045362ull
//   6443204691850407923ull -> 7634998654808690043ull
//   1658944240032470522ull -> 3845678135899710043ull
//   4564528776249873855ull -> 60626724102493512ull
//   17316063992144767815ull -> 16503481641668561908ull
//   1138673537377377155ull -> 15040106242869486882ull
//   4984525601846280238ull -> 1213586276935313252ull
//   8694866053676819856ull -> 7105826286621370478ull
//   1584577151820503907ull -> 13080487245400062203ull
//   7336426762353074103ull -> 9900065397881108365ull
//   4297651875135187523ull -> 11175928055869418645ull
//   3358546249625000219ull -> 15196193893632458680ull
//   9774203748618497442ull -> 12574930759015574656ull
//   15023261739455876001ull -> 180229384902392595ull
//   17982483925466052345ull -> 14931811156527589396ull
//   12067274609566293803ull -> 7025769596000376474ull
//   2795487836124372222ull -> 11128902767468582530ull
//   16924141763831892887ull -> 17287768040640926785ull
//   10758346952141907762ull -> 8046156127041051839ull
//   2311051631285001948ull -> 2576933522647501972ull
//   16941051314076490383ull -> 11516981055053986601ull
//   8023409289033289423ull -> 15040561571098226314ull
//   10902677858936058066ull -> 5215032498854786682ull
//   13443126914813445573ull -> 15426973044609261854ull
//   5715540528381471121ull -> 6732542386604522331ull
//   11598049666469522901ull -> 3501344427354627919ull
//   6439204370199141165ull -> 17375367742411431301ull
//   4888547293499339163ull -> 1739880003141880127ull
//   6866984268264696985ull -> 2467030153255185580ull
//   7480253846593316468ull -> 13404512392582016370ull
//   6866984268264696985ull -> 2467030153255185580ull
// expectation corrected (v19-E, k_stream_aborted, 25 of 35 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   948783147963005660ull -> 7287402210134811723ull
//   17513437510661292093ull -> 3613812001802944490ull
//   14992337489397360393ull -> 14256716415753092846ull
//   4371503310698045362ull -> 18066710111273402560ull
//   7634998654808690043ull -> 14450906895180287465ull
//   60626724102493512ull -> 7810034545682398155ull
//   16503481641668561908ull -> 15431396809014267622ull
//   1213586276935313252ull -> 15078256075606303379ull
//   13080487245400062203ull -> 807892153055512972ull
//   9900065397881108365ull -> 8496224644929899065ull
//   11175928055869418645ull -> 17064616757066975641ull
//   12574930759015574656ull -> 7052726630314129836ull
//   180229384902392595ull -> 8205504961151388183ull
//   14931811156527589396ull -> 1165523721440006008ull
//   7025769596000376474ull -> 10509778316010069121ull
//   17287768040640926785ull -> 12820182186070573599ull
//   8046156127041051839ull -> 1003285434194249605ull
//   11516981055053986601ull -> 9459669056269242935ull
//   5215032498854786682ull -> 16298324343950560662ull
//   15426973044609261854ull -> 7295767240173679712ull
//   6732542386604522331ull -> 11303232892540595619ull
//   3501344427354627919ull -> 9108531251772622477ull
//   1739880003141880127ull -> 13980272045865766065ull
//   2467030153255185580ull -> 7431106375660941195ull
//   2467030153255185580ull -> 7431106375660941195ull
// expectation corrected (V19-FIX, k_stream_aborted, 35 of 35 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   7287402210134811723ull -> 16510252855932086823ull
//   6258262929575333087ull -> 14048324983519873828ull
//   3613812001802944490ull -> 17804079663198632649ull
//   14256716415753092846ull -> 7665081442761407167ull
//   18066710111273402560ull -> 1510562666313522433ull
//   14450906895180287465ull -> 16253369998080909938ull
//   3845678135899710043ull -> 15976513930905472129ull
//   7810034545682398155ull -> 3846751227738950664ull
//   15431396809014267622ull -> 15450124388103584652ull
//   15040106242869486882ull -> 2225665425051551577ull
//   15078256075606303379ull -> 15615312821753524918ull
//   7105826286621370478ull -> 11958084298410326056ull
//   807892153055512972ull -> 7978187604899367877ull
//   8496224644929899065ull -> 876738446875159256ull
//   17064616757066975641ull -> 3498362567022607364ull
//   15196193893632458680ull -> 7053403205296459880ull
//   7052726630314129836ull -> 11858356529053237ull
//   8205504961151388183ull -> 5137924053122990913ull
//   1165523721440006008ull -> 1455779883697863925ull
//   10509778316010069121ull -> 1450972605221627516ull
//   11128902767468582530ull -> 6748150359618158603ull
//   12820182186070573599ull -> 17100694157255179539ull
//   1003285434194249605ull -> 16383805058165184155ull
//   2576933522647501972ull -> 14458762612387161134ull
//   9459669056269242935ull -> 7578874640041129857ull
//   15040561571098226314ull -> 1091721997205765161ull
//   16298324343950560662ull -> 4972829120680344696ull
//   7295767240173679712ull -> 11905106519757767589ull
//   11303232892540595619ull -> 14990642672847258155ull
//   9108531251772622477ull -> 18051484560037243770ull
//   17375367742411431301ull -> 7846354589731725875ull
//   13980272045865766065ull -> 1584652476433904101ull
//   7431106375660941195ull -> 12788838416146221578ull
//   13404512392582016370ull -> 6600350751158217016ull
//   7431106375660941195ull -> 12788838416146221578ull
// expectation corrected (INT25, k_stream_aborted, 35 of 35 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   16510252855932086823ull -> 16458853630937199693ull
//   14048324983519873828ull -> 10534014544629119321ull
//   17804079663198632649ull -> 15852567477617018112ull
//   7665081442761407167ull -> 8395254103899656747ull
//   1510562666313522433ull -> 9137153746257222586ull
//   16253369998080909938ull -> 4321243615010347791ull
//   15976513930905472129ull -> 2464381025353933752ull
//   3846751227738950664ull -> 4335032133758631355ull
//   15450124388103584652ull -> 3418642895666580588ull
//   2225665425051551577ull -> 12043850726481513634ull
//   15615312821753524918ull -> 15778603109418817912ull
//   11958084298410326056ull -> 7422979898823356625ull
//   7978187604899367877ull -> 8082887755714141501ull
//   876738446875159256ull -> 18272796975887582060ull
//   3498362567022607364ull -> 4620349392656757831ull
//   7053403205296459880ull -> 11062470475981718812ull
//   11858356529053237ull -> 11681022048666484124ull
//   5137924053122990913ull -> 1110410394930231379ull
//   1455779883697863925ull -> 8353289200898942510ull
//   1450972605221627516ull -> 5464661787014092283ull
//   6748150359618158603ull -> 18042314908697358845ull
//   17100694157255179539ull -> 17913569146247237990ull
//   16383805058165184155ull -> 14252084119842247317ull
//   14458762612387161134ull -> 13951402111806527659ull
//   7578874640041129857ull -> 14984524306684102418ull
//   1091721997205765161ull -> 4929388494852177725ull
//   4972829120680344696ull -> 8427843384471180011ull
//   11905106519757767589ull -> 12312569890916686665ull
//   14990642672847258155ull -> 7806471840435792838ull
//   18051484560037243770ull -> 10841176947711978698ull
//   7846354589731725875ull -> 14141658027994245481ull
//   1584652476433904101ull -> 15796138674937755121ull
//   12788838416146221578ull -> 4930499034900633496ull
//   6600350751158217016ull -> 6596242839819495123ull
//   12788838416146221578ull -> 4930499034900633496ull
// expectation corrected (INT26 v19 hash re-pin, k_stream_aborted, 25 of 35 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   16458853630937199693ull -> 6675987432147888000ull [H-THIN]
//   15852567477617018112ull -> 15316836834528354089ull [H-THIN]
//   8395254103899656747ull -> 122347678355491576ull [H-THIN]
//   9137153746257222586ull -> 17060189170921966279ull [H-THIN]
//   4321243615010347791ull -> 12998233506120003610ull [H-THIN]
//   4335032133758631355ull -> 5790160451626852554ull [H-THIN]
//   3418642895666580588ull -> 8747488118396803171ull [H-THIN]
//   15778603109418817912ull -> 11301555486053984887ull [H-THIN]
//   8082887755714141501ull -> 16019190197976852534ull [H-THIN]
//   18272796975887582060ull -> 14554266247005678391ull [H-THIN]
//   4620349392656757831ull -> 9863308761378043570ull [H-THIN]
//   11681022048666484124ull -> 3981212442403576087ull [H-THIN]
//   1110410394930231379ull -> 16893712934576968012ull [H-THIN]
//   8353289200898942510ull -> 17103134812120181635ull [H-THIN]
//   5464661787014092283ull -> 2716679541586828074ull [H-THIN]
//   17913569146247237990ull -> 2142815117417671925ull [H-THIN]
//   14252084119842247317ull -> 1297101144475427900ull [H-THIN]
//   14984524306684102418ull -> 17860190683404402179ull [H-THIN]
//   8427843384471180011ull -> 12743930332679229528ull [H-THIN]
//   12312569890916686665ull -> 13672064593010625780ull [H-THIN]
//   7806471840435792838ull -> 15322331001617686957ull [H-THIN]
//   10841176947711978698ull -> 2540452936526860029ull [H-THIN]
//   15796138674937755121ull -> 5707160760001918266ull [H-THIN]
//   4930499034900633496ull -> 17256519628932910437ull [H-THIN]
//   4930499034900633496ull -> 17256519628932910437ull [H-THIN]
constexpr std::uint64_t k_stream_aborted[] = {
    6675987432147888000ull,
    10534014544629119321ull,
    15316836834528354089ull,
    122347678355491576ull,
    17060189170921966279ull,
    12998233506120003610ull,
    2464381025353933752ull,
    5790160451626852554ull,
    8747488118396803171ull,
    12043850726481513634ull,
    11301555486053984887ull,
    7422979898823356625ull,
    16019190197976852534ull,
    14554266247005678391ull,
    9863308761378043570ull,
    11062470475981718812ull,
    3981212442403576087ull,
    16893712934576968012ull,
    17103134812120181635ull,
    2716679541586828074ull,
    18042314908697358845ull,
    2142815117417671925ull,
    1297101144475427900ull,
    13951402111806527659ull,
    17860190683404402179ull,
    4929388494852177725ull,
    12743930332679229528ull,
    13672064593010625780ull,
    15322331001617686957ull,
    2540452936526860029ull,
    14141658027994245481ull,
    5707160760001918266ull,
    17256519628932910437ull,
    6596242839819495123ull,
    17256519628932910437ull,
};
// expectation corrected (k_aborted, 6 of 6 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   2196150024938657215ull -> 2543779130970217232ull
//   12315990772160430197ull -> 5674505149482193397ull
//   17118064270458636817ull -> 9408409355545209490ull
//   8759006371174325107ull -> 3319916503755701546ull
//   2321311737097736302ull -> 13980550444579133815ull
//   8759006371174325107ull -> 3319916503755701546ull
// expectation corrected (v19-B, k_aborted, 5 of 6 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   5674505149482193397ull -> 18435485463499933528ull
//   9408409355545209490ull -> 17904934979399943420ull
//   3319916503755701546ull -> 16410967088932597466ull
//   13980550444579133815ull -> 4177885075058309128ull
//   3319916503755701546ull -> 16410967088932597466ull
// expectation corrected (v19-E, k_aborted, 3 of 6 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   18435485463499933528ull -> 2482407147974245352ull
//   16410967088932597466ull -> 7856356957291164459ull
//   16410967088932597466ull -> 7856356957291164459ull
// expectation corrected (V19-FIX, k_aborted, 6 of 6 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   2543779130970217232ull -> 14621553252499744072ull
//   2482407147974245352ull -> 1560069015253079270ull
//   17904934979399943420ull -> 8181232920474966978ull
//   7856356957291164459ull -> 12415576349368044635ull
//   4177885075058309128ull -> 14564863596030526264ull
//   7856356957291164459ull -> 12415576349368044635ull
// expectation corrected (INT25, k_aborted, 6 of 6 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   14621553252499744072ull -> 5575980828577033360ull
//   1560069015253079270ull -> 17778912298194596574ull
//   8181232920474966978ull -> 16463834114134690168ull
//   12415576349368044635ull -> 12754404695042992156ull
//   14564863596030526264ull -> 8825354032181240290ull
//   12415576349368044635ull -> 12754404695042992156ull
// expectation corrected (INT26 v19 hash re-pin, k_aborted, 3 of 6 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   17778912298194596574ull -> 2540222023230692795ull [H-THIN]
//   12754404695042992156ull -> 8965275888317139301ull [H-THIN]
//   12754404695042992156ull -> 8965275888317139301ull [H-THIN]
constexpr std::uint64_t k_aborted[] = {
    5575980828577033360ull,
    2540222023230692795ull,
    16463834114134690168ull,
    8965275888317139301ull,
    8825354032181240290ull,
    8965275888317139301ull,
};
// expectation corrected (k_reused, 15 of 15 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   2196150024938657215ull -> 2543779130970217232ull
//   12315990772160430197ull -> 5674505149482193397ull
//   17118064270458636817ull -> 9408409355545209490ull
//   16843348278421860698ull -> 12820592968519298463ull
//   2196150024938657215ull -> 2543779130970217232ull
//   12315990772160430197ull -> 5674505149482193397ull
//   17118064270458636817ull -> 9408409355545209490ull
//   16843348278421860698ull -> 12820592968519298463ull
//   7897887485017793412ull -> 4834660818464838173ull
//   9578444851499107596ull -> 5623648009772522091ull
//   7897887485017793412ull -> 4834660818464838173ull
//   3177959751229611117ull -> 11044589305457306293ull
//   6736841173273024464ull -> 4297651875135187523ull
//   8024070780969239705ull -> 15283171904605073866ull
//   6736841173273024464ull -> 4297651875135187523ull
// expectation corrected (v19-B, k_reused, 13 of 15 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   5674505149482193397ull -> 18435485463499933528ull
//   9408409355545209490ull -> 17904934979399943420ull
//   12820592968519298463ull -> 14580145596766604804ull
//   5674505149482193397ull -> 18435485463499933528ull
//   9408409355545209490ull -> 17904934979399943420ull
//   12820592968519298463ull -> 14580145596766604804ull
//   4834660818464838173ull -> 12763565106297155564ull
//   5623648009772522091ull -> 10324478866479362969ull
//   4834660818464838173ull -> 12763565106297155564ull
//   11044589305457306293ull -> 5493020030945000624ull
//   4297651875135187523ull -> 11175928055869418645ull
//   15283171904605073866ull -> 15316717102556938983ull
//   4297651875135187523ull -> 11175928055869418645ull
// expectation corrected (v19-E, k_reused, 9 of 15 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   18435485463499933528ull -> 2482407147974245352ull
//   14580145596766604804ull -> 7253611708149050520ull
//   18435485463499933528ull -> 2482407147974245352ull
//   14580145596766604804ull -> 7253611708149050520ull
//   12763565106297155564ull -> 13208968445305919179ull
//   12763565106297155564ull -> 13208968445305919179ull
//   5493020030945000624ull -> 4221562876897277718ull
//   11175928055869418645ull -> 17064616757066975641ull
//   11175928055869418645ull -> 17064616757066975641ull
// expectation corrected (V19-FIX, k_reused, 15 of 15 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   2543779130970217232ull -> 14621553252499744072ull
//   2482407147974245352ull -> 1560069015253079270ull
//   17904934979399943420ull -> 8181232920474966978ull
//   7253611708149050520ull -> 8150352125352055815ull
//   2543779130970217232ull -> 14621553252499744072ull
//   2482407147974245352ull -> 1560069015253079270ull
//   17904934979399943420ull -> 8181232920474966978ull
//   7253611708149050520ull -> 8150352125352055815ull
//   13208968445305919179ull -> 4370795498617211977ull
//   10324478866479362969ull -> 10361542891827113526ull
//   13208968445305919179ull -> 4370795498617211977ull
//   4221562876897277718ull -> 1258618990550900905ull
//   17064616757066975641ull -> 3498362567022607364ull
//   15316717102556938983ull -> 9810780513184341561ull
//   17064616757066975641ull -> 3498362567022607364ull
// expectation corrected (INT25, k_reused, 15 of 15 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   14621553252499744072ull -> 5575980828577033360ull
//   1560069015253079270ull -> 17778912298194596574ull
//   8181232920474966978ull -> 16463834114134690168ull
//   8150352125352055815ull -> 8941018855024971093ull
//   14621553252499744072ull -> 5575980828577033360ull
//   1560069015253079270ull -> 17778912298194596574ull
//   8181232920474966978ull -> 16463834114134690168ull
//   8150352125352055815ull -> 8941018855024971093ull
//   4370795498617211977ull -> 2898399952537902865ull
//   10361542891827113526ull -> 8964175343669296125ull
//   4370795498617211977ull -> 2898399952537902865ull
//   1258618990550900905ull -> 4063572268643314481ull
//   3498362567022607364ull -> 4620349392656757831ull
//   9810780513184341561ull -> 7040750059417794475ull
//   3498362567022607364ull -> 4620349392656757831ull
// expectation corrected (INT26 v19 hash re-pin, k_reused, 9 of 15 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   17778912298194596574ull -> 2540222023230692795ull [H-THIN]
//   8941018855024971093ull -> 7277266162097189602ull [H-THIN]
//   17778912298194596574ull -> 2540222023230692795ull [H-THIN]
//   8941018855024971093ull -> 7277266162097189602ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
//   4063572268643314481ull -> 3045160088509885646ull [H-THIN]
//   4620349392656757831ull -> 9863308761378043570ull [H-THIN]
//   4620349392656757831ull -> 9863308761378043570ull [H-THIN]
constexpr std::uint64_t k_reused[] = {
    5575980828577033360ull,
    2540222023230692795ull,
    16463834114134690168ull,
    7277266162097189602ull,
    5575980828577033360ull,
    2540222023230692795ull,
    16463834114134690168ull,
    7277266162097189602ull,
    13929697350787785222ull,
    8964175343669296125ull,
    13929697350787785222ull,
    3045160088509885646ull,
    9863308761378043570ull,
    7040750059417794475ull,
    9863308761378043570ull,
};
// expectation corrected (k_refused, 11 of 11 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   9578444851499107596ull -> 5623648009772522091ull
//   7897887485017793412ull -> 4834660818464838173ull
//   9578444851499107596ull -> 5623648009772522091ull
//   7897887485017793412ull -> 4834660818464838173ull
//   2196150024938657215ull -> 2543779130970217232ull
//   12315990772160430197ull -> 5674505149482193397ull
//   17118064270458636817ull -> 9408409355545209490ull
//   16843348278421860698ull -> 12820592968519298463ull
//   7897887485017793412ull -> 4834660818464838173ull
//   9578444851499107596ull -> 5623648009772522091ull
//   7897887485017793412ull -> 4834660818464838173ull
// expectation corrected (v19-B, k_refused, 10 of 11 values), because v19-B folds each replace successor's chain root once, at the replace, into the continuation (the core's chain index, the cohort state the journal window no longer carries):
//   5623648009772522091ull -> 10324478866479362969ull
//   4834660818464838173ull -> 12763565106297155564ull
//   5623648009772522091ull -> 10324478866479362969ull
//   4834660818464838173ull -> 12763565106297155564ull
//   5674505149482193397ull -> 18435485463499933528ull
//   9408409355545209490ull -> 17904934979399943420ull
//   12820592968519298463ull -> 14580145596766604804ull
//   4834660818464838173ull -> 12763565106297155564ull
//   5623648009772522091ull -> 10324478866479362969ull
//   4834660818464838173ull -> 12763565106297155564ull
// expectation corrected (v19-E, k_refused, 6 of 11 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with -DPINEFORGE_P1_HARVEST against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272; these are the reads V19-E moved on its own base, and where V19-B's chain-root fold moved a read too the value takes both folds, so it differs from V19-E's tip there:
//   12763565106297155564ull -> 13208968445305919179ull
//   12763565106297155564ull -> 13208968445305919179ull
//   18435485463499933528ull -> 2482407147974245352ull
//   14580145596766604804ull -> 7253611708149050520ull
//   12763565106297155564ull -> 13208968445305919179ull
//   12763565106297155564ull -> 13208968445305919179ull
// expectation corrected (V19-FIX, k_refused, 11 of 11 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   10324478866479362969ull -> 10361542891827113526ull
//   13208968445305919179ull -> 4370795498617211977ull
//   10324478866479362969ull -> 10361542891827113526ull
//   13208968445305919179ull -> 4370795498617211977ull
//   2543779130970217232ull -> 14621553252499744072ull
//   2482407147974245352ull -> 1560069015253079270ull
//   17904934979399943420ull -> 8181232920474966978ull
//   7253611708149050520ull -> 8150352125352055815ull
//   13208968445305919179ull -> 4370795498617211977ull
//   10324478866479362969ull -> 10361542891827113526ull
//   13208968445305919179ull -> 4370795498617211977ull
// expectation corrected (INT25, k_refused, 11 of 11 values), because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch:
//   10361542891827113526ull -> 8964175343669296125ull
//   4370795498617211977ull -> 2898399952537902865ull
//   10361542891827113526ull -> 8964175343669296125ull
//   4370795498617211977ull -> 2898399952537902865ull
//   14621553252499744072ull -> 5575980828577033360ull
//   1560069015253079270ull -> 17778912298194596574ull
//   8181232920474966978ull -> 16463834114134690168ull
//   8150352125352055815ull -> 8941018855024971093ull
//   4370795498617211977ull -> 2898399952537902865ull
//   10361542891827113526ull -> 8964175343669296125ull
//   4370795498617211977ull -> 2898399952537902865ull
// expectation corrected (INT26 v19 hash re-pin, k_refused, 6 of 11 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary:
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
//   17778912298194596574ull -> 2540222023230692795ull [H-THIN]
//   8941018855024971093ull -> 7277266162097189602ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
//   2898399952537902865ull -> 13929697350787785222ull [H-THIN]
constexpr std::uint64_t k_refused[] = {
    8964175343669296125ull,
    13929697350787785222ull,
    8964175343669296125ull,
    13929697350787785222ull,
    5575980828577033360ull,
    2540222023230692795ull,
    16463834114134690168ull,
    7277266162097189602ull,
    13929697350787785222ull,
    8964175343669296125ull,
    13929697350787785222ull,
};
// P1_PINNED_DATA_END

struct Pinned {
    const std::uint64_t* reads;
    std::size_t len;
};

const Pinned kPinned[] = {
    {k_chart, std::size(k_chart)},
    {k_magnifier, std::size(k_magnifier)},
    {k_aggregated, std::size(k_aggregated)},
    {k_aggregated_magnifier, std::size(k_aggregated_magnifier)},
    {k_coof, std::size(k_coof)},
    {k_recording, std::size(k_recording)},
    {k_stream, std::size(k_stream)},
    {k_stream_aborted, std::size(k_stream_aborted)},
    {k_aborted, std::size(k_aborted)},
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
void same_reads(const char* scenario, const char* leg, const std::vector<std::uint64_t>& got,
                const std::uint64_t* want, std::size_t want_len) {
    CHECK(got.size() == want_len);
    for (std::size_t i = 0; i < got.size() && i < want_len; ++i) {
        if (got[i] != want[i]) {
            std::fprintf(stderr, "%s %s read[%zu]: observed %llu, want %llu\n", scenario, leg, i,
                         static_cast<unsigned long long>(got[i]),
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
    for (const auto& scenario : kScenarios) emit(scenario.name, scenario.run(Options{}));
    return 0;
#else
    for (std::size_t s = 0; s < std::size(kScenarios); ++s) {
        const auto& scenario = kScenarios[s];
        same_reads(scenario.name, "run 1", scenario.run(Options{}), kPinned[s].reads,
                   kPinned[s].len);
        same_reads(scenario.name, "run 2", scenario.run(Options{}), kPinned[s].reads,
                   kPinned[s].len);
    }
    // The pins are non-trivial: each scenario reads at least three values.
    for (const auto& pinned : kPinned) CHECK(distinct(pinned) >= 3);
    if (failures == 0)
        std::printf("test_adapter_continuation_view: %d checks, 0 failures\n", checks);
    return failures == 0 ? 0 : 1;
#endif
}
