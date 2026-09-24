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
constexpr std::uint64_t k_chart[] = {
    2543779130970217232ull,
    2482407147974245352ull,
    17904934979399943420ull,
    7253611708149050520ull,
    13208968445305919179ull,
    10324478866479362969ull,
    13208968445305919179ull,
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
constexpr std::uint64_t k_magnifier[] = {
    2055499121853794215ull,
    14570450431216633494ull,
    12633570209086223403ull,
    3037174366487220809ull,
    4411147221463257853ull,
    11201307460949430058ull,
    4411147221463257853ull,
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
constexpr std::uint64_t k_aggregated[] = {
    7408940017323371281ull,
    10940756508045292790ull,
    5992953984750558008ull,
    5456021430637490595ull,
    5992953984750558008ull,
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
constexpr std::uint64_t k_aggregated_magnifier[] = {
    2376229218543853263ull,
    2877250321201605922ull,
    18112393076487589516ull,
    13595293692156698476ull,
    18112393076487589516ull,
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
constexpr std::uint64_t k_coof[] = {
    8160477046829279565ull,
    8683725853452377701ull,
    16612330879755983674ull,
    3284793633481963595ull,
    9898269111674054383ull,
    15200193571573177943ull,
    220118684227198753ull,
    5365537423784642334ull,
    17202960931486753898ull,
    5365537423784642334ull,
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
constexpr std::uint64_t k_recording[] = {
    2543779130970217232ull,
    13638291719838344805ull,
    17904934979399943420ull,
    4916994404502736826ull,
    13208968445305919179ull,
    10324478866479362969ull,
    13208968445305919179ull,
    10364728431969872992ull,
    6773723984513471634ull,
    17279077540384140970ull,
    273555295542222474ull,
    8791634334263737749ull,
    925668679100948855ull,
    8777619534496248594ull,
    17725129497555317017ull,
    8008837844251753449ull,
    5594741258697960485ull,
    15079181317456407482ull,
    17089349116617098209ull,
    18200208521056000396ull,
    15241045489297703683ull,
    1762199573452402758ull,
    11385708691232883797ull,
    17964225683996893310ull,
    4534429732864628476ull,
    17467596193696644527ull,
    16401650137151877883ull,
    16808577086136594190ull,
    12964209703146465173ull,
    16463685165528796935ull,
    17915487794021341896ull,
    15978106025216492785ull,
    1109289945107352897ull,
    15892759426148401507ull,
    17112119348245367605ull,
    8581843276479262047ull,
    7131344654937855853ull,
    14792600582710493575ull,
    1239841573570926230ull,
    5444729147284661655ull,
    3245433697010211721ull,
    4676939050322725348ull,
    7350099411866253542ull,
    343377133147017054ull,
    9598996651795533367ull,
    14030542661451411676ull,
    9219466268922236720ull,
    6348273631916125388ull,
    1051431539591183352ull,
    2350462348068176311ull,
    5268079867744237915ull,
    2325247658527992044ull,
    6174548207444471958ull,
    8596767173582221714ull,
    986987187009250324ull,
    13994596646214270165ull,
    2602688672529104322ull,
    3911601850678352661ull,
    9593288478406285213ull,
    4236346333119319709ull,
    15363079072004362933ull,
    18107900300087896759ull,
    17921584433192374312ull,
    5816305515259473479ull,
    10970209394144352158ull,
    7950770882156242927ull,
    18330351249071377885ull,
    13975661199209811620ull,
    11989522059105970117ull,
    14967623866548664000ull,
    13208968445305919179ull,
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
constexpr std::uint64_t k_stream[] = {
    7287402210134811723ull,
    6258262929575333087ull,
    3613812001802944490ull,
    14256716415753092846ull,
    18066710111273402560ull,
    14450906895180287465ull,
    3845678135899710043ull,
    7810034545682398155ull,
    15431396809014267622ull,
    15040106242869486882ull,
    15078256075606303379ull,
    7105826286621370478ull,
    807892153055512972ull,
    8496224644929899065ull,
    17064616757066975641ull,
    15196193893632458680ull,
    7052726630314129836ull,
    8205504961151388183ull,
    1165523721440006008ull,
    10509778316010069121ull,
    11128902767468582530ull,
    12820182186070573599ull,
    1003285434194249605ull,
    2576933522647501972ull,
    9459669056269242935ull,
    15040561571098226314ull,
    16298324343950560662ull,
    7295767240173679712ull,
    11303232892540595619ull,
    9108531251772622477ull,
    17375367742411431301ull,
    13980272045865766065ull,
    7431106375660941195ull,
    8791307287609743160ull,
    14145180498168891911ull,
    11868273362524112458ull,
    9898567439601994650ull,
    2093231171095230746ull,
    5084138944843925215ull,
    15691180810974081764ull,
    4498955898635597792ull,
    16786848328132643741ull,
    18123708192449447342ull,
    4395122555036264306ull,
    10159557292487317173ull,
    12231697623938131977ull,
    5309579489344409296ull,
    8546285193301548912ull,
    6138899047874310618ull,
    10443374725851587239ull,
    5882164565997988658ull,
    10443374725851587239ull,
    11033218740417797382ull,
    10443374725851587239ull,
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
constexpr std::uint64_t k_stream_aborted[] = {
    7287402210134811723ull,
    6258262929575333087ull,
    3613812001802944490ull,
    14256716415753092846ull,
    18066710111273402560ull,
    14450906895180287465ull,
    3845678135899710043ull,
    7810034545682398155ull,
    15431396809014267622ull,
    15040106242869486882ull,
    15078256075606303379ull,
    7105826286621370478ull,
    807892153055512972ull,
    8496224644929899065ull,
    17064616757066975641ull,
    15196193893632458680ull,
    7052726630314129836ull,
    8205504961151388183ull,
    1165523721440006008ull,
    10509778316010069121ull,
    11128902767468582530ull,
    12820182186070573599ull,
    1003285434194249605ull,
    2576933522647501972ull,
    9459669056269242935ull,
    15040561571098226314ull,
    16298324343950560662ull,
    7295767240173679712ull,
    11303232892540595619ull,
    9108531251772622477ull,
    17375367742411431301ull,
    13980272045865766065ull,
    7431106375660941195ull,
    13404512392582016370ull,
    7431106375660941195ull,
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
constexpr std::uint64_t k_aborted[] = {
    2543779130970217232ull,
    2482407147974245352ull,
    17904934979399943420ull,
    7856356957291164459ull,
    4177885075058309128ull,
    7856356957291164459ull,
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
constexpr std::uint64_t k_reused[] = {
    2543779130970217232ull,
    2482407147974245352ull,
    17904934979399943420ull,
    7253611708149050520ull,
    2543779130970217232ull,
    2482407147974245352ull,
    17904934979399943420ull,
    7253611708149050520ull,
    13208968445305919179ull,
    10324478866479362969ull,
    13208968445305919179ull,
    4221562876897277718ull,
    17064616757066975641ull,
    15316717102556938983ull,
    17064616757066975641ull,
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
constexpr std::uint64_t k_refused[] = {
    10324478866479362969ull,
    13208968445305919179ull,
    10324478866479362969ull,
    13208968445305919179ull,
    2543779130970217232ull,
    2482407147974245352ull,
    17904934979399943420ull,
    7253611708149050520ull,
    13208968445305919179ull,
    10324478866479362969ull,
    13208968445305919179ull,
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
