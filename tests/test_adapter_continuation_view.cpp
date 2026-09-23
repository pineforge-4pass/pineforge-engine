// R5 lane PERF-P1: a Pine run's terminal continuation, latched as a view and
// folded on first read, answers the eager capture's value at every read.
//
// PineStrategyHost::capture_script_continuation_hash latches the
// continuation at the run's last script point -- the last batch bar, a
// leftover input after an aggregated chart's last bucket, every realtime bar
// of a stream -- so that broker_state_hash() is one value with recording on
// or off. That latch was one full fold, taken whether or not anything read
// it; it now is a view of the fold (NativeExecutionConsumer::
// capture_continuation_view) unless a recorded row needs the value at once.
//
// The witness runs every scenario twice, on hosts that differ only in the
// consumer's defer_continuation_views switch -- off, the view is folded at
// capture, which is the eager capture itself -- and asserts that every read
// agrees: reads from inside the script (the live fold and the latch), after
// Completed, after the next begin of a reused host, after a refused begin and
// after an abort; the recorded rows too, under recording. The drivings cover
// every capture site: chart timeframe, bar magnifier, an aggregated chart
// with leftover input (with and without the magnifier), calc_on_order_fills,
// recording on, a stream's warmup and realtime legs, and a stream that aborts.
// The Deferred values are also pinned against the base library, and a
// Deferred batch run returns with its view still pending: the fold it used to
// pay was not paid.
//
// Portability. The syminfo zone is the fixed offset "UTC+0", which the
// resolver answers from its definition alone, so no tzdata file enters the
// continuation (E23) and the pins hold on every host. Every price is an exact
// binary fraction on the 0.25 tick.
//
// Provenance of the pinned data: this TU compiled unchanged against the base
// library (engine main fc7aad62) with -DPINEFORGE_P1_HARVEST, which runs the
// base library's (eager) capture only and prints the observed values as the
// initializers below. Rebuild them the same way; never edit one by hand.
//
// Fail-before (fc7aad62), first diagnostic without the harvest switch:
//   tests/test_adapter_continuation_view.cpp:106:20: error: no member named
//   'defer_continuation_views' in 'pineforge::NativeExecutionConsumer'
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"

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
    bool deferred = true;
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
#ifndef PINEFORGE_P1_HARVEST
        consumer().defer_continuation_views(options.deferred);
#endif
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

#ifndef PINEFORGE_P1_HARVEST
    bool view_pending() { return consumer().continuation_view_pending(); }

private:
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
#endif
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

// Recording on: the rows need the value at once, so the capture is eager in
// both legs; the scalar and every row still agree.
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
// inside the script read views, some after a live fold moved past them.
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

// One host: a batch whose latch is never read (the next begin drops it), a
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

// A begin refused before it resets anything leaves the latch standing: the
// unread view reads after the refusal, the live fold first; then the host
// runs again.
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
constexpr std::uint64_t k_chart[] = {
    2543779130970217232ull,
    5674505149482193397ull,
    9408409355545209490ull,
    12820592968519298463ull,
    4834660818464838173ull,
    5623648009772522091ull,
    4834660818464838173ull,
};
// expectation corrected (k_magnifier, 7 of 7 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   10072028075855707823ull -> 2055499121853794215ull
//   8569312294347288489ull -> 703801249040575285ull
//   2632522011360028001ull -> 14872490446707053531ull
//   9681067599565431733ull -> 7510587436020870414ull
//   6545773095595780937ull -> 13457869248821340102ull
//   13485101370296523681ull -> 4055821710073734035ull
//   6545773095595780937ull -> 13457869248821340102ull
constexpr std::uint64_t k_magnifier[] = {
    2055499121853794215ull,
    703801249040575285ull,
    14872490446707053531ull,
    7510587436020870414ull,
    13457869248821340102ull,
    4055821710073734035ull,
    13457869248821340102ull,
};
// expectation corrected (k_aggregated, 5 of 5 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   15579341700060222430ull -> 7408940017323371281ull
//   14994859925119476497ull -> 17786335956283624301ull
//   16532888688512789671ull -> 4311320861969680288ull
//   3620459343021292889ull -> 5456021430637490595ull
//   16532888688512789671ull -> 4311320861969680288ull
constexpr std::uint64_t k_aggregated[] = {
    7408940017323371281ull,
    17786335956283624301ull,
    4311320861969680288ull,
    5456021430637490595ull,
    4311320861969680288ull,
};
// expectation corrected (k_aggregated_magnifier, 5 of 5 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   12594241837591534357ull -> 2376229218543853263ull
//   10703578186271721676ull -> 11362706686723722160ull
//   16309698280952568764ull -> 8515292501914737744ull
//   2257923986347215325ull -> 13595293692156698476ull
//   16309698280952568764ull -> 8515292501914737744ull
constexpr std::uint64_t k_aggregated_magnifier[] = {
    2376229218543853263ull,
    11362706686723722160ull,
    8515292501914737744ull,
    13595293692156698476ull,
    8515292501914737744ull,
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
constexpr std::uint64_t k_coof[] = {
    8160477046829279565ull,
    8683725853452377701ull,
    16612330879755983674ull,
    5948900213206369390ull,
    3940848663703711224ull,
    5449918228566479877ull,
    6135396872829398814ull,
    7598518073669438847ull,
    6938517794080095754ull,
    7598518073669438847ull,
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
constexpr std::uint64_t k_recording[] = {
    2543779130970217232ull,
    12102871947415603019ull,
    9408409355545209490ull,
    9731921227394455728ull,
    4834660818464838173ull,
    5623648009772522091ull,
    4834660818464838173ull,
    7390183078382216635ull,
    4275052578147536997ull,
    9782579203668197445ull,
    18137986040855890756ull,
    1127752528809105344ull,
    460621270365484909ull,
    10729940105119008355ull,
    6719506549743237672ull,
    9601497081148438203ull,
    10071572065205276794ull,
    7652585746495678617ull,
    5510227761230738907ull,
    6482743127232178518ull,
    9757172817451018200ull,
    11782218145116696034ull,
    4331588667985227456ull,
    4031221698890367954ull,
    7316513615807483167ull,
    7887806825697949620ull,
    14369960460099008479ull,
    1445832673687625444ull,
    3065788277782737739ull,
    16777199233526513054ull,
    12817867269153787474ull,
    9059503499681960574ull,
    13426438431872780295ull,
    13422469892124742512ull,
    2796634976022197618ull,
    7177790782070250822ull,
    12809920657886367582ull,
    6026131998021598350ull,
    14582561950966654444ull,
    8404845127637694823ull,
    17365191549808574849ull,
    13491912311498866261ull,
    11277843155689476212ull,
    9811981650000812306ull,
    13216014769651389651ull,
    12670664983955043063ull,
    12211597308880478155ull,
    11647447651341082476ull,
    2575673101403584766ull,
    2026106225486543071ull,
    17977577869004360273ull,
    2648136582573533333ull,
    4474418605383541641ull,
    738361606928178595ull,
    4322246313805691111ull,
    17334629200554089976ull,
    8098927344052802931ull,
    1380943936535676434ull,
    14453082372026923489ull,
    4600079008853518937ull,
    3786255565884902157ull,
    969620699942930924ull,
    15715946055027507243ull,
    14464190903244952662ull,
    5527657694788709574ull,
    5715618086727856832ull,
    4457180920033346810ull,
    7239452625558369345ull,
    17262674001658265772ull,
    14147667235407649041ull,
    4834660818464838173ull,
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
constexpr std::uint64_t k_stream[] = {
    9926360377267477400ull,
    6983879323613695038ull,
    15722806537016766703ull,
    3639719031233734366ull,
    6715353660937589514ull,
    6443204691850407923ull,
    1658944240032470522ull,
    4564528776249873855ull,
    17316063992144767815ull,
    1138673537377377155ull,
    4984525601846280238ull,
    8694866053676819856ull,
    1584577151820503907ull,
    7336426762353074103ull,
    4297651875135187523ull,
    3358546249625000219ull,
    9774203748618497442ull,
    15023261739455876001ull,
    17982483925466052345ull,
    12067274609566293803ull,
    2795487836124372222ull,
    16924141763831892887ull,
    10758346952141907762ull,
    2311051631285001948ull,
    16941051314076490383ull,
    8023409289033289423ull,
    10902677858936058066ull,
    13443126914813445573ull,
    5715540528381471121ull,
    11598049666469522901ull,
    6439204370199141165ull,
    4888547293499339163ull,
    6866984268264696985ull,
    3759587071855175591ull,
    10133115201077270549ull,
    18414563990090124433ull,
    788358065976043576ull,
    632402651546170885ull,
    10146430786139564947ull,
    16999736537802937080ull,
    6008469009194404982ull,
    1504430507355741288ull,
    11660732259755016903ull,
    15629776065187965767ull,
    561208533436643181ull,
    4727655756242265723ull,
    1093697520232017686ull,
    14400352229660723061ull,
    18171012142792015032ull,
    15414161288850079440ull,
    5839767826071636333ull,
    15414161288850079440ull,
    4057136666456172998ull,
    15414161288850079440ull,
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
constexpr std::uint64_t k_stream_aborted[] = {
    9926360377267477400ull,
    6983879323613695038ull,
    15722806537016766703ull,
    3639719031233734366ull,
    6715353660937589514ull,
    6443204691850407923ull,
    1658944240032470522ull,
    4564528776249873855ull,
    17316063992144767815ull,
    1138673537377377155ull,
    4984525601846280238ull,
    8694866053676819856ull,
    1584577151820503907ull,
    7336426762353074103ull,
    4297651875135187523ull,
    3358546249625000219ull,
    9774203748618497442ull,
    15023261739455876001ull,
    17982483925466052345ull,
    12067274609566293803ull,
    2795487836124372222ull,
    16924141763831892887ull,
    10758346952141907762ull,
    2311051631285001948ull,
    16941051314076490383ull,
    8023409289033289423ull,
    10902677858936058066ull,
    13443126914813445573ull,
    5715540528381471121ull,
    11598049666469522901ull,
    6439204370199141165ull,
    4888547293499339163ull,
    6866984268264696985ull,
    7480253846593316468ull,
    6866984268264696985ull,
};
// expectation corrected (k_aborted, 6 of 6 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19):
//   2196150024938657215ull -> 2543779130970217232ull
//   12315990772160430197ull -> 5674505149482193397ull
//   17118064270458636817ull -> 9408409355545209490ull
//   8759006371174325107ull -> 3319916503755701546ull
//   2321311737097736302ull -> 13980550444579133815ull
//   8759006371174325107ull -> 3319916503755701546ull
constexpr std::uint64_t k_aborted[] = {
    2543779130970217232ull,
    5674505149482193397ull,
    9408409355545209490ull,
    3319916503755701546ull,
    13980550444579133815ull,
    3319916503755701546ull,
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
constexpr std::uint64_t k_reused[] = {
    2543779130970217232ull,
    5674505149482193397ull,
    9408409355545209490ull,
    12820592968519298463ull,
    2543779130970217232ull,
    5674505149482193397ull,
    9408409355545209490ull,
    12820592968519298463ull,
    4834660818464838173ull,
    5623648009772522091ull,
    4834660818464838173ull,
    11044589305457306293ull,
    4297651875135187523ull,
    15283171904605073866ull,
    4297651875135187523ull,
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
constexpr std::uint64_t k_refused[] = {
    5623648009772522091ull,
    4834660818464838173ull,
    5623648009772522091ull,
    4834660818464838173ull,
    2543779130970217232ull,
    5674505149482193397ull,
    9408409355545209490ull,
    12820592968519298463ull,
    4834660818464838173ull,
    5623648009772522091ull,
    4834660818464838173ull,
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

// The first read that moved, so a failing view names the point.
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
    Options eager_leg;
    eager_leg.deferred = false;
    for (std::size_t s = 0; s < std::size(kScenarios); ++s) {
        const auto& scenario = kScenarios[s];
        const auto eager = scenario.run(eager_leg);
        const auto deferred = scenario.run(Options{});
        // The core: the deferred view answers the eager capture at every read.
        same_reads(scenario.name, "Deferred-vs-Eager", deferred, eager.data(), eager.size());
        // And the values are the base library's, read for read.
        same_reads(scenario.name, "Deferred-vs-pin", deferred, kPinned[s].reads, kPinned[s].len);
    }
    // The fold is really deferred: after a batch run the view is still
    // pending, and a read folds it; the eager leg has nothing left to fold.
    {
        ViewStrategy deferred(Options{});
        const auto bars = tape(kBars);
        deferred.run(bars.data(), kBars);
        CHECK(deferred.view_pending());
        deferred.read_now();
        CHECK(!deferred.view_pending());
        ViewStrategy eager(eager_leg);
        eager.run(bars.data(), kBars);
        CHECK(!eager.view_pending());
        CHECK(eager.broker_state_hash() == deferred.broker_state_hash());
    }
    // The pins are non-trivial: each scenario reads at least three values.
    for (const auto& pinned : kPinned) CHECK(distinct(pinned) >= 3);
    if (failures == 0)
        std::printf("test_adapter_continuation_view: %d checks, 0 failures\n", checks);
    return failures == 0 ? 0 : 1;
#endif
}
