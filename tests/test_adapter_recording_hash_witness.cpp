// R5 lane F9: the recording-mode broker-state hash of a Pine run, value by
// value, at every observation point.
//
// With set_broker_state_hash_recording(true) a Pine host appends one
// broker_state_hash() per script bar, and every one of those folds the
// adapter's whole retained source state (pine_state_hash.cpp): every
// placement snapshot the run ever accepted, the admission journal and the
// consumed source prefix. Folding all of it again at every bar is what makes
// recording quadratic in the bar count (lane F9, both final audits). Any
// cheaper fold -- a cache per placement, an immutable prefix folded once --
// has to answer the SAME value at every point a hash is read, including the
// points where a retained row has just been rewritten in place: a replaced
// working order, an exit that re-issues every bar, a cancel receipt, an OCA
// sibling cancelled by a fill, a trail that arms, and an entry that filled
// long ago and is only now given its bracket (in lane F9's probe its row's
// has_full_entry_bracket flips 59 bars after the fill, at the attach).
// A reset -- the same host running again -- must answer the fresh values.
//
// So this witness is data: every recorded row, a read after every command
// the scenario issues, and the final scalar, for six scenarios, observed on
// engine main fd785928 and pinned here. It says nothing about cost; it is
// what an incremental fold must reproduce bit for bit.
//
// Portability. A recorded row folds the consumer's continuation, and that
// folds the zone's tzdata content (E23), which is not a pin this row wants.
// The host below overrides the projection to fold one fixed execution hash
// instead, exactly as test_adapter_report_relower.cpp does, so what remains
// is the kernel's broker state and the whole source extension -- the part a
// cheaper adapter fold would change. Every price is an exact binary fraction
// on a 0.25 tick, so every hashed double is reproducible to the bit.
//
// Provenance of the pinned data: this TU, compiled unchanged against the
// fd785928 library with -DPINEFORGE_F9_HARVEST (which prints the observed
// values as the initializers below instead of checking them). Rebuild them
// the same way; never edit one by hand to make a run pass.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
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

constexpr std::uint64_t kProbeExecutionHash = 0x5eed1234abcd00f9ull;
constexpr std::int64_t T = 1736121600000LL;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

enum class Scenario { Submit, Replace, Cancel, Oca, Reissue, Trail };

const char* name_of(Scenario scenario) {
    switch (scenario) {
    case Scenario::Submit: return "Submit";
    case Scenario::Replace: return "Replace";
    case Scenario::Cancel: return "Cancel";
    case Scenario::Oca: return "Oca";
    case Scenario::Reissue: return "Reissue";
    case Scenario::Trail: return "Trail";
    }
    return "?";
}

// A triangular wave in quarter steps (every price on the 0.25 tick), or a
// ramp up then down for the trail.
double price_at(Scenario scenario, int index) {
    if (scenario == Scenario::Trail)
        return index < 20 ? 100.0 + 0.25 * index : 105.0 - 0.25 * (index - 20);
    const int phase = index % 8;
    const int triangle = phase < 4 ? phase : 8 - phase;
    return 100.0 + 0.5 * triangle + 0.25 * (index % 3);
}

constexpr int kBars = 32;

std::vector<Bar> feed(Scenario scenario) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(kBars));
    for (int i = 0; i < kBars; ++i) {
        const double p = price_at(scenario, i);
        bars.push_back({p, p + 0.5, p - 0.5, p + 0.25, 1.0,
                        T + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

// A generated-strategy-shaped source host: the body issues Pine commands and
// reads the hash after each one.
class WitnessHost final : public source::PineStrategyHost {
public:
    WitnessHost(Scenario scenario, bool recording) : scenario_(scenario) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 1.0;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(recording);
    }

    std::uint64_t broker_state_hash_projection() const override {
        return broker_state_hash_from_execution_hash(kProbeExecutionHash);
    }

    std::vector<std::uint64_t> reads;

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        switch (scenario_) {
        case Scenario::Submit: submit(i); break;
        case Scenario::Replace: replace(i); break;
        case Scenario::Cancel: cancel(i); break;
        case Scenario::Oca: oca(i, bar); break;
        case Scenario::Reissue: reissue(i, bar); break;
        case Scenario::Trail: trail(i); break;
        }
    }

private:
    void read() { reads.push_back(broker_state_hash()); }

    // Market, resting limit and stop entries accepted, filled and closed.
    void submit(int i) {
        if (i == 1) { strategy_entry("A", true); read(); }
        if (i == 3) { strategy_entry("B", true, 100.0); read(); }
        if (i == 10) { strategy_close("A"); read(); }
        if (i == 16) { strategy_close_all(); read(); }
        if (i == 20) { strategy_entry("D", true, kNa, 102.0); read(); }
        if (i == 26) { strategy_close("D"); read(); }
    }

    // One working entry re-priced four times, then filled; its exit is
    // re-priced once before it fills.
    void replace(int i) {
        if (i >= 1 && i <= 5) { strategy_entry("L", true, 99.0 + 0.25 * (i - 1)); read(); }
        if (i == 7) { strategy_entry("L", true, 100.25); read(); }
        if (i == 12) { strategy_exit("x", "L", 102.5, 98.0); read(); }
        if (i == 14) { strategy_exit("x", "L", 101.25, 98.0); read(); }
    }

    // Working orders cancelled by id and all at once; the same-bar submit and
    // cancel of the audit probe; a cancel that arrives after the fill.
    void cancel(int i) {
        if (i == 1) { strategy_order("bid", true, 1, 95.0); read(); }
        if (i == 4) { strategy_cancel("bid"); read(); }
        if (i == 5) {
            strategy_entry("E", true, 95.0); read();
            strategy_order("o2", true, 1, 95.5); read();
        }
        if (i == 8) { strategy_cancel_all(); read(); }
        if (i >= 10 && i <= 20) {
            strategy_order("bid", true, 1, 1.0);
            strategy_cancel("bid"); read();
        }
        if (i == 22) { strategy_entry("F", true); read(); }
        if (i == 24) { strategy_cancel("F"); read(); }
        if (i == 28) { strategy_close("F"); read(); }
    }

    // Two cancel-group entries per cycle: the first fill cancels its
    // sibling. The last cycle is a reduce group.
    void oca(int i, const Bar& bar) {
        const int phase = i % 8;
        const bool reduce = i >= 24;
        const char* group = reduce ? "r" : "g";
        const int type = reduce ? 2 : 1;
        if (phase == 1) {
            strategy_entry("A", true, bar.close - 0.25, kNa, kNa, {}, group, type); read();
            strategy_entry("B", true, bar.close - 0.75, kNa, kNa, {}, group, type); read();
        }
        if (phase == 6) { strategy_close_all(); read(); }
    }

    // An entry issued at bar 0 fills at bar 1 and is given its first exit
    // only at bar 8; the exit then re-issues every bar, and its last re-issue
    // raises the stop into the tape, where it fills. A second entry
    // re-issues its exit to the end of the tape and is still open there.
    void reissue(int i, const Bar& bar) {
        if (i == 0) { strategy_entry("L", true); read(); }
        if (i >= 8 && i < 20 && live_position_size() > 0.0) {
            strategy_exit("guard", "L", bar.close + 3.0, bar.close - 3.0); read();
        }
        if (i == 20) { strategy_exit("guard", "L", bar.close + 0.25, bar.close - 3.0); read(); }
        if (i == 24) { strategy_entry("M", true); read(); }
        if (i >= 25) { strategy_exit("keep", "M", bar.close + 3.0, bar.close - 3.0); read(); }
    }

    // A trailing exit issued once and left to arm and follow the ramp.
    void trail(int i) {
        if (i == 0) { strategy_entry("L", true); read(); }
        if (i == 2) { strategy_exit("t", "L", kNa, kNa, 4.0, 2.0); read(); }
    }

    Scenario scenario_;
};

struct Trade {
    std::int64_t entry_time;
    std::int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    int open_at_end;
};

struct Observed {
    std::vector<std::uint64_t> rows;
    std::vector<std::uint64_t> reads;
    std::uint64_t final_hash = 0;
    std::vector<Trade> trades;
};

Observed collect(WitnessHost& host, Scenario scenario) {
    host.reads.clear();
    const auto bars = feed(scenario);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    Observed out;
    ReportC report{};
    host.fill_report(&report);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        out.rows.push_back(report.broker_state_hash[i]);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        out.trades.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price,
                              t.qty, t.open_at_end});
    }
    BacktestEngine::free_report(&report);
    out.reads = host.reads;
    out.final_hash = host.broker_state_hash();
    return out;
}

Observed observe(Scenario scenario, bool recording = true) {
    WitnessHost host(scenario, recording);
    return collect(host, scenario);
}

#ifdef PINEFORGE_F9_HARVEST
constexpr Scenario kScenarios[] = {Scenario::Submit, Scenario::Replace, Scenario::Cancel,
                                   Scenario::Oca, Scenario::Reissue, Scenario::Trail};

void emit(Scenario scenario, const Observed& got) {
    const char* symbol = name_of(scenario);
    std::printf("constexpr std::uint64_t k%s_rows[] = {\n", symbol);
    for (const auto h : got.rows) std::printf("    %lluull,\n", static_cast<unsigned long long>(h));
    std::printf("};\nconstexpr std::uint64_t k%s_reads[] = {\n", symbol);
    for (const auto h : got.reads) std::printf("    %lluull,\n", static_cast<unsigned long long>(h));
    std::printf("};\nconstexpr std::uint64_t k%s_final = %lluull;\n", symbol,
                static_cast<unsigned long long>(got.final_hash));
    std::printf("constexpr Trade k%s_trades[] = {\n", symbol);
    for (const auto& t : got.trades)
        std::printf("    {%lldLL, %lldLL, %.17g, %.17g, %.17g, %d},\n",
                    static_cast<long long>(t.entry_time), static_cast<long long>(t.exit_time),
                    t.entry_price, t.exit_price, t.qty, t.open_at_end);
    std::printf("};\n\n");
}
#else
// ── Pinned data (see the provenance note at the top of this file) ───────
// F9_PINNED_DATA_BEGIN
// expectation corrected (kSubmit_rows, 32 of 32 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   16709085573648202138ull -> 16509221787135873163ull
//   2367456619966436912ull -> 1111427372116971977ull
//   4494419410966626025ull -> 10113084496075445606ull
//   17380701833127534467ull -> 2823618685552948560ull
//   451619535107290686ull -> 9344428235602768525ull
//   2187293916418065859ull -> 4413101517248877980ull
//   4012538314749493373ull -> 1882066454334115146ull
//   10866247629710005993ull -> 17141329699489201238ull
//   17310929364016147338ull -> 1083258955716083227ull
//   12956826091814529166ull -> 9068467176826596759ull
//   9622783738306862249ull -> 7677225212687412544ull
//   8622803854841958074ull -> 905410631039502871ull
//   4970368351605491565ull -> 8799950975279705652ull
//   8890797197972884845ull -> 13950087688800027348ull
//   5428705957761616462ull -> 9256228887281681175ull
//   8977647938089996694ull -> 15215453593001383255ull
//   13807565145524110216ull -> 16133558601105106997ull
//   2685926045958892414ull -> 4809994858186250859ull
//   681270338635768226ull -> 565119218453387419ull
//   1234383668290826547ull -> 8621259759080457014ull
//   4756780818784761844ull -> 2249277129240130089ull
//   4691618259015575702ull -> 9017239805943870213ull
//   16054237718346851917ull -> 14905307869633859146ull
//   16534350264022081281ull -> 6768769721692064218ull
//   14658458716123961306ull -> 1679803940186180185ull
//   5627465258600142271ull -> 8513081312183242332ull
//   331903815701686263ull -> 4886268333774479680ull
//   4813090721847088203ull -> 17953510043011989047ull
//   12071930564723693486ull -> 2518322017114092290ull
//   7270151308451508039ull -> 8364385477967995923ull
//   2993593795212311439ull -> 3220915131726720827ull
//   4550614867809308423ull -> 6186330499779649827ull
// expectation corrected (v19-E, kSubmit_rows, 32 of 32 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   16509221787135873163ull -> 16885932052227463976ull
//   1111427372116971977ull -> 1832414573103394886ull
//   10113084496075445606ull -> 6662732155889098474ull
//   2823618685552948560ull -> 16935908075746659915ull
//   9344428235602768525ull -> 8670184406548067167ull
//   4413101517248877980ull -> 17311759586000019611ull
//   1882066454334115146ull -> 5110535151530170856ull
//   17141329699489201238ull -> 17050466140112728026ull
//   1083258955716083227ull -> 4513821336585640874ull
//   9068467176826596759ull -> 9506584848066022008ull
//   7677225212687412544ull -> 34481296660224287ull
//   905410631039502871ull -> 16226829862460102889ull
//   8799950975279705652ull -> 5715736824005291633ull
//   13950087688800027348ull -> 16672763487273857144ull
//   9256228887281681175ull -> 11194061843429059943ull
//   15215453593001383255ull -> 2985158813983697164ull
//   16133558601105106997ull -> 14754578871476587428ull
//   4809994858186250859ull -> 1606482689304043337ull
//   565119218453387419ull -> 5495385592042698151ull
//   8621259759080457014ull -> 2522232666137668356ull
//   2249277129240130089ull -> 4085198162701970644ull
//   9017239805943870213ull -> 7117747547444830272ull
//   14905307869633859146ull -> 14444814725304782334ull
//   6768769721692064218ull -> 1707217372995824821ull
//   1679803940186180185ull -> 12266542813062942244ull
//   8513081312183242332ull -> 7182805577633544037ull
//   4886268333774479680ull -> 10829573004894599276ull
//   17953510043011989047ull -> 7789554680157984885ull
//   2518322017114092290ull -> 13305448560089262767ull
//   8364385477967995923ull -> 11423048270061715692ull
//   3220915131726720827ull -> 13118814681321380879ull
//   6186330499779649827ull -> 8426887344131973464ull
// expectation corrected (V19-FIX, kSubmit_rows, 32 of 32 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   16885932052227463976ull -> 8933562200260968840ull
//   1832414573103394886ull -> 16496088491414191334ull
//   6662732155889098474ull -> 17745030456223078154ull
//   16935908075746659915ull -> 15976454321452418827ull
//   8670184406548067167ull -> 13867778238676275167ull
//   17311759586000019611ull -> 10620212436062486555ull
//   5110535151530170856ull -> 16387014991078439144ull
//   17050466140112728026ull -> 5372310558802353946ull
//   4513821336585640874ull -> 8117928017652874474ull
//   9506584848066022008ull -> 12227480580368604280ull
//   34481296660224287ull -> 11671431865371234655ull
//   16226829862460102889ull -> 15852753888607827401ull
//   5715736824005291633ull -> 4564599765874863441ull
//   16672763487273857144ull -> 12753515022268993432ull
//   11194061843429059943ull -> 16693007526387479527ull
//   2985158813983697164ull -> 9553609781489681132ull
//   14754578871476587428ull -> 9723295982254899748ull
//   1606482689304043337ull -> 9596448656160987273ull
//   5495385592042698151ull -> 14172117111020274407ull
//   2522232666137668356ull -> 1811404642983946404ull
//   4085198162701970644ull -> 6600438489637577460ull
//   7117747547444830272ull -> 1823325663201613856ull
//   14444814725304782334ull -> 671600216917334430ull
//   1707217372995824821ull -> 14450998569007950837ull
//   12266542813062942244ull -> 18123601071981623076ull
//   7182805577633544037ull -> 5788029478896071429ull
//   10829573004894599276ull -> 5791781074908951980ull
//   7789554680157984885ull -> 8180458203668078357ull
//   13305448560089262767ull -> 10842259428116090863ull
//   11423048270061715692ull -> 7789401651194802796ull
//   13118814681321380879ull -> 16644199069697802031ull
//   8426887344131973464ull -> 13033963886965614456ull
// expectation corrected (INT26 v19 hash re-pin, kSubmit_rows, 32 of 32 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   8933562200260968840ull -> 8019365971493599225ull [H-THIN]
//   16496088491414191334ull -> 12084928604454138979ull [H-THIN]
//   17745030456223078154ull -> 7685642069387429171ull [H-THIN]
//   15976454321452418827ull -> 3048458165749351592ull [H-THIN]
//   13867778238676275167ull -> 1016691552216785494ull [H-THIN]
//   10620212436062486555ull -> 13664491885780078814ull [H-THIN]
//   16387014991078439144ull -> 9587023827432894395ull [H-THIN]
//   5372310558802353946ull -> 14524900109868328621ull [H-THIN]
//   8117928017652874474ull -> 9306730010360799799ull [H-THIN]
//   12227480580368604280ull -> 10975560481515534245ull [H-THIN]
//   11671431865371234655ull -> 6175700847996002068ull [H-THIN]
//   15852753888607827401ull -> 17944617882018938484ull [H-THIN]
//   4564599765874863441ull -> 89066156438191170ull [H-THIN]
//   12753515022268993432ull -> 7853223159133263199ull [H-THIN]
//   16693007526387479527ull -> 16829337964629633742ull [H-THIN]
//   9553609781489681132ull -> 14048972284943154657ull [H-THIN]
//   9723295982254899748ull -> 10772403295793355383ull [H-THIN]
//   9596448656160987273ull -> 12849842554292919416ull [H-THIN]
//   14172117111020274407ull -> 11523841829240280476ull [H-THIN]
//   1811404642983946404ull -> 14633701048963800807ull [H-THIN]
//   6600438489637577460ull -> 13106625599621486697ull [H-THIN]
//   1823325663201613856ull -> 11578248645351712284ull [H-THIN]
//   671600216917334430ull -> 12923518206466935124ull [H-THIN]
//   14450998569007950837ull -> 11664001542269768811ull [H-THIN]
//   18123601071981623076ull -> 4353270054101958588ull [H-THIN]
//   5788029478896071429ull -> 12509431446261158271ull [H-THIN]
//   5791781074908951980ull -> 6827814975693211880ull [H-THIN]
//   8180458203668078357ull -> 13768158546548443068ull [H-THIN]
//   10842259428116090863ull -> 6634357882333049268ull [H-THIN]
//   7789401651194802796ull -> 8521102792149394239ull [H-THIN]
//   16644199069697802031ull -> 15674750399051881772ull [H-THIN]
//   13033963886965614456ull -> 7695240294002266835ull [H-THIN]
constexpr std::uint64_t kSubmit_rows[] = {
    8019365971493599225ull,
    12084928604454138979ull,
    7685642069387429171ull,
    3048458165749351592ull,
    1016691552216785494ull,
    13664491885780078814ull,
    9587023827432894395ull,
    14524900109868328621ull,
    9306730010360799799ull,
    10975560481515534245ull,
    6175700847996002068ull,
    17944617882018938484ull,
    89066156438191170ull,
    7853223159133263199ull,
    16829337964629633742ull,
    14048972284943154657ull,
    10772403295793355383ull,
    12849842554292919416ull,
    11523841829240280476ull,
    14633701048963800807ull,
    13106625599621486697ull,
    11578248645351712284ull,
    12923518206466935124ull,
    11664001542269768811ull,
    4353270054101958588ull,
    12509431446261158271ull,
    6827814975693211880ull,
    13768158546548443068ull,
    6634357882333049268ull,
    8521102792149394239ull,
    15674750399051881772ull,
    7695240294002266835ull,
};
// expectation corrected (kSubmit_reads, 6 of 6 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   10242542257977841923ull -> 17535387619041092918ull
//   16540927499285076903ull -> 14347748759156271488ull
//   6945018333701386048ull -> 12106641332320457033ull
//   7356558553176868704ull -> 15777849557804947821ull
//   11860308250432984141ull -> 4845782216021612032ull
//   5117593015133294249ull -> 10452897047571570010ull
// expectation corrected (v19-E, kSubmit_reads, 6 of 6 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   17535387619041092918ull -> 10505763622515464111ull
//   14347748759156271488ull -> 7912407605268438972ull
//   12106641332320457033ull -> 14277317571452203719ull
//   15777849557804947821ull -> 3006894002872600101ull
//   4845782216021612032ull -> 14061760729711229018ull
//   10452897047571570010ull -> 11455159794740779404ull
// expectation corrected (V19-FIX, kSubmit_reads, 6 of 6 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   10505763622515464111ull -> 5783964658104710927ull
//   7912407605268438972ull -> 9790324176931117308ull
//   14277317571452203719ull -> 8640216615515382983ull
//   3006894002872600101ull -> 18016456059351763749ull
//   14061760729711229018ull -> 6161691521401770042ull
//   11455159794740779404ull -> 11652988695302303084ull
// expectation corrected (INT26 v19 hash re-pin, kSubmit_reads, 6 of 6 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   5783964658104710927ull -> 18234150623076098218ull [H-THIN]
//   9790324176931117308ull -> 7528636846310271579ull [H-THIN]
//   8640216615515382983ull -> 6832312803948695488ull [H-THIN]
//   18016456059351763749ull -> 11582188679677093386ull [H-THIN]
//   6161691521401770042ull -> 13475769493769981375ull [H-THIN]
//   11652988695302303084ull -> 10681991124354674742ull [H-THIN]
constexpr std::uint64_t kSubmit_reads[] = {
    18234150623076098218ull,
    7528636846310271579ull,
    6832312803948695488ull,
    11582188679677093386ull,
    13475769493769981375ull,
    10681991124354674742ull,
};
// expectation corrected: kSubmit_final 4550614867809308423ull -> 6186330499779649827ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move.
// expectation corrected (v19-E): kSubmit_final 6186330499779649827ull -> 8426887344131973464ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
// expectation corrected (V19-FIX): kSubmit_final 8426887344131973464ull -> 13033963886965614456ull, because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree.
// expectation corrected (INT26 v19 hash re-pin): kSubmit_final 13033963886965614456ull -> 7695240294002266835ull [H-THIN], because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move.
constexpr std::uint64_t kSubmit_final = 7695240294002266835ull;
constexpr Trade kSubmit_trades[] = {
    {1736121720000LL, 1736122260000LL, 101.5, 102, 2, 0},
    {1736122080000LL, 1736122620000LL, 100, 101, 2, 0},
    {1736122860000LL, 1736123220000LL, 102, 101.5, 2, 0},
};

// expectation corrected (kReplace_rows, 32 of 32 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   16709085573648202138ull -> 16509221787135873163ull
//   15983598378715930306ull -> 3329559763866374491ull
//   4269588758895930903ull -> 7952436146494761830ull
//   11036824734939475591ull -> 126386027871859294ull
//   12257656879758622505ull -> 6465097981428867416ull
//   14411164098109175801ull -> 6536605762225726676ull
//   9450757305780161736ull -> 12590188922876073805ull
//   7551970176279275465ull -> 13230357688865415236ull
//   14453287925757798752ull -> 16165480584133435319ull
//   12976323852396269092ull -> 472451229442797971ull
//   5805001876393475955ull -> 7629433744296385120ull
//   10844959506524389683ull -> 9937109344977821592ull
//   13291722376204817347ull -> 2479229512455873680ull
//   11465826076836594596ull -> 8877637488493929411ull
//   11893948688539042135ull -> 11946727129360043876ull
//   8910467690674179911ull -> 5055474805996246416ull
//   12771933610374359049ull -> 6501427085718734274ull
//   11122028775837490723ull -> 12733901498347385787ull
//   7037834018034259618ull -> 5414253509409789338ull
//   17539147401022057209ull -> 6735884170978307857ull
//   5656548508765946794ull -> 763755966996372290ull
//   16066430581990033300ull -> 16035709127218074412ull
//   13615537378955925529ull -> 12913856157958874769ull
//   9849716321944625332ull -> 5060510264541901452ull
//   9702250351256038096ull -> 17449515293282034344ull
//   4449732415226067191ull -> 160756582023226335ull
//   9601235874939509895ull -> 10852926514061334799ull
//   13771220425752823706ull -> 13323564334294945170ull
//   13034380409707450981ull -> 16504913721580145725ull
//   10371866353517103554ull -> 3960465168252133786ull
//   11781937174085148984ull -> 6210507725413841856ull
//   17912935963855979970ull -> 2578691877718696634ull
// expectation corrected (v19-E, kReplace_rows, 32 of 32 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   16509221787135873163ull -> 16885932052227463976ull
//   3329559763866374491ull -> 17547842047240632273ull
//   7952436146494761830ull -> 13652197343159488211ull
//   126386027871859294ull -> 6374388350102285126ull
//   6465097981428867416ull -> 1467195078376742887ull
//   6536605762225726676ull -> 11968361826170869554ull
//   12590188922876073805ull -> 4128651056395996559ull
//   13230357688865415236ull -> 15934564140823409182ull
//   16165480584133435319ull -> 11303720518502300590ull
//   472451229442797971ull -> 11904649057339333172ull
//   7629433744296385120ull -> 11048733698266497284ull
//   9937109344977821592ull -> 16674821699842999755ull
//   2479229512455873680ull -> 16437822789666463784ull
//   8877637488493929411ull -> 2093244317622874278ull
//   11946727129360043876ull -> 12114907797348974860ull
//   5055474805996246416ull -> 3352113756351333150ull
//   6501427085718734274ull -> 9656428624069983049ull
//   12733901498347385787ull -> 18340684566969229321ull
//   5414253509409789338ull -> 13581693612191648950ull
//   6735884170978307857ull -> 2146300135587413999ull
//   763755966996372290ull -> 2784997083595937551ull
//   16035709127218074412ull -> 6656116134246569845ull
//   12913856157958874769ull -> 2967091859373217631ull
//   5060510264541901452ull -> 7218379802597322779ull
//   17449515293282034344ull -> 4433033628872207973ull
//   160756582023226335ull -> 18318911876151815972ull
//   10852926514061334799ull -> 7063083486589508720ull
//   13323564334294945170ull -> 1398286556357525565ull
//   16504913721580145725ull -> 14302900766917140512ull
//   3960465168252133786ull -> 1078279461778315641ull
//   6210507725413841856ull -> 13355807928615532752ull
//   2578691877718696634ull -> 836327192465797433ull
// expectation corrected (V19-FIX, kReplace_rows, 32 of 32 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   16885932052227463976ull -> 8933562200260968840ull
//   17547842047240632273ull -> 3907924323473264241ull
//   13652197343159488211ull -> 14461884941578386867ull
//   6374388350102285126ull -> 16511157315480471622ull
//   1467195078376742887ull -> 3959552939271361991ull
//   11968361826170869554ull -> 2555551956738490898ull
//   4128651056395996559ull -> 10502649610878884111ull
//   15934564140823409182ull -> 18306494604560422814ull
//   11303720518502300590ull -> 17185448195161859406ull
//   11904649057339333172ull -> 15450494133515205524ull
//   11048733698266497284ull -> 6827766899990563012ull
//   16674821699842999755ull -> 181096906100466027ull
//   16437822789666463784ull -> 3679474327996900584ull
//   2093244317622874278ull -> 5841986886798320038ull
//   12114907797348974860ull -> 1819868140276584460ull
//   3352113756351333150ull -> 4420902119403666462ull
//   9656428624069983049ull -> 13480284355483224649ull
//   18340684566969229321ull -> 7175317954498263753ull
//   13581693612191648950ull -> 17073623445577360118ull
//   2146300135587413999ull -> 17009991270755579471ull
//   2784997083595937551ull -> 12090343301005594287ull
//   6656116134246569845ull -> 13041638478193314581ull
//   2967091859373217631ull -> 1923525511758336863ull
//   7218379802597322779ull -> 1774789224381390235ull
//   4433033628872207973ull -> 15509873019280549637ull
//   18318911876151815972ull -> 18049355505317406500ull
//   7063083486589508720ull -> 9113574443316558064ull
//   1398286556357525565ull -> 52835018303832477ull
//   14302900766917140512ull -> 17600220618483225632ull
//   1078279461778315641ull -> 4090055281926467129ull
//   13355807928615532752ull -> 10973342611545588080ull
//   836327192465797433ull -> 17124982032285163865ull
// expectation corrected (INT26 v19 hash re-pin, kReplace_rows, 32 of 32 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   8933562200260968840ull -> 8019365971493599225ull [H-THIN]
//   3907924323473264241ull -> 10913970432357026228ull [H-THIN]
//   14461884941578386867ull -> 16718022224308212524ull [H-THIN]
//   16511157315480471622ull -> 11959962526167016269ull [H-THIN]
//   3959552939271361991ull -> 13682103540524835918ull [H-THIN]
//   2555551956738490898ull -> 14243367393985337845ull [H-THIN]
//   10502649610878884111ull -> 8014600618664019672ull [H-THIN]
//   18306494604560422814ull -> 12889133748772252011ull [H-THIN]
//   17185448195161859406ull -> 360570764097142565ull [H-THIN]
//   15450494133515205524ull -> 11876071708761659347ull [H-THIN]
//   6827766899990563012ull -> 3851347914344196379ull [H-THIN]
//   181096906100466027ull -> 269582453071901974ull [H-THIN]
//   3679474327996900584ull -> 7374511927462938135ull [H-THIN]
//   5841986886798320038ull -> 14141590806976750289ull [H-THIN]
//   1819868140276584460ull -> 10009811972588039873ull [H-THIN]
//   4420902119403666462ull -> 6680845581363570511ull [H-THIN]
//   13480284355483224649ull -> 14019640910801526228ull [H-THIN]
//   7175317954498263753ull -> 8435723009555847478ull [H-THIN]
//   17073623445577360118ull -> 11806787700855741945ull [H-THIN]
//   17009991270755579471ull -> 4186867479979939080ull [H-THIN]
//   12090343301005594287ull -> 4211449273197151184ull [H-THIN]
//   13041638478193314581ull -> 2665513011878476638ull [H-THIN]
//   1923525511758336863ull -> 6197194006704225848ull [H-THIN]
//   1774789224381390235ull -> 9112509013863788708ull [H-THIN]
//   15509873019280549637ull -> 8040469185102305882ull [H-THIN]
//   18049355505317406500ull -> 2771278037748002351ull [H-THIN]
//   9113574443316558064ull -> 11922167282207411627ull [H-THIN]
//   52835018303832477ull -> 11390247996551829374ull [H-THIN]
//   17600220618483225632ull -> 5206095883568772047ull [H-THIN]
//   4090055281926467129ull -> 2965585984607700150ull [H-THIN]
//   10973342611545588080ull -> 14433906025888897687ull [H-THIN]
//   17124982032285163865ull -> 8339014417427264918ull [H-THIN]
constexpr std::uint64_t kReplace_rows[] = {
    8019365971493599225ull,
    10913970432357026228ull,
    16718022224308212524ull,
    11959962526167016269ull,
    13682103540524835918ull,
    14243367393985337845ull,
    8014600618664019672ull,
    12889133748772252011ull,
    360570764097142565ull,
    11876071708761659347ull,
    3851347914344196379ull,
    269582453071901974ull,
    7374511927462938135ull,
    14141590806976750289ull,
    10009811972588039873ull,
    6680845581363570511ull,
    14019640910801526228ull,
    8435723009555847478ull,
    11806787700855741945ull,
    4186867479979939080ull,
    4211449273197151184ull,
    2665513011878476638ull,
    6197194006704225848ull,
    9112509013863788708ull,
    8040469185102305882ull,
    2771278037748002351ull,
    11922167282207411627ull,
    11390247996551829374ull,
    5206095883568772047ull,
    2965585984607700150ull,
    14433906025888897687ull,
    8339014417427264918ull,
};
// expectation corrected (kReplace_reads, 8 of 8 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   8242046592519750921ull -> 5144918420709740188ull
//   14188906364679548382ull -> 18001364646555756511ull
//   6575148672512319035ull -> 15425907159865944850ull
//   8722165113924581213ull -> 1507111833987561188ull
//   10537654301970811988ull -> 7645803652552624941ull
//   16831556078795966924ull -> 4839696889052561061ull
//   3306904984544048232ull -> 11379263290234193203ull
//   3367765439354791003ull -> 10164053459169499104ull
// expectation corrected (v19-E, kReplace_reads, 8 of 8 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   5144918420709740188ull -> 6332508079166398204ull
//   18001364646555756511ull -> 8080051585834415455ull
//   15425907159865944850ull -> 18331632290663282909ull
//   1507111833987561188ull -> 3083764297320273482ull
//   7645803652552624941ull -> 1018849945992994654ull
//   4839696889052561061ull -> 1583090386514829653ull
//   11379263290234193203ull -> 10955971990176778399ull
//   10164053459169499104ull -> 5763828109735666890ull
// expectation corrected (V19-FIX, kReplace_reads, 8 of 8 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   6332508079166398204ull -> 2326526227949002076ull
//   8080051585834415455ull -> 13390056939270062271ull
//   18331632290663282909ull -> 10021657182331917789ull
//   3083764297320273482ull -> 14242637685443801194ull
//   1018849945992994654ull -> 711594472462182014ull
//   1583090386514829653ull -> 4857350833193818325ull
//   10955971990176778399ull -> 4317633836685601759ull
//   5763828109735666890ull -> 1066029825997878730ull
// expectation corrected (INT26 v19 hash re-pin, kReplace_reads, 8 of 8 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   2326526227949002076ull -> 18240752413274260441ull [H-THIN]
//   13390056939270062271ull -> 2691016625363928396ull [H-THIN]
//   10021657182331917789ull -> 6810061420294520706ull [H-THIN]
//   14242637685443801194ull -> 9524522377897439163ull [H-THIN]
//   711594472462182014ull -> 7296154269922229361ull [H-THIN]
//   4857350833193818325ull -> 17144192060924016500ull [H-THIN]
//   4317633836685601759ull -> 15841551483731080184ull [H-THIN]
//   1066029825997878730ull -> 3093248295129869043ull [H-THIN]
constexpr std::uint64_t kReplace_reads[] = {
    18240752413274260441ull,
    2691016625363928396ull,
    6810061420294520706ull,
    9524522377897439163ull,
    7296154269922229361ull,
    17144192060924016500ull,
    15841551483731080184ull,
    3093248295129869043ull,
};
// expectation corrected: kReplace_final 17912935963855979970ull -> 2578691877718696634ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move.
// expectation corrected (v19-E): kReplace_final 2578691877718696634ull -> 836327192465797433ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
// expectation corrected (V19-FIX): kReplace_final 836327192465797433ull -> 17124982032285163865ull, because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree.
// expectation corrected (INT26 v19 hash re-pin): kReplace_final 17124982032285163865ull -> 8339014417427264918ull [H-THIN], because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move.
constexpr std::uint64_t kReplace_final = 8339014417427264918ull;
constexpr Trade kReplace_trades[] = {
    {1736122080000LL, 1736122620000LL, 100.25, 101.25, 2, 0},
};

// expectation corrected (kCancel_rows, 32 of 32 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   16709085573648202138ull -> 16509221787135873163ull
//   17453591279558544262ull -> 12162968985552207795ull
//   17575092659149777791ull -> 4269270195170776294ull
//   3211348821023947102ull -> 4853599967980404227ull
//   13458899738611866827ull -> 14437417135330127222ull
//   17722996774723384595ull -> 10586829955756583920ull
//   6428562136194339281ull -> 12081162804397252102ull
//   13084510485948270853ull -> 8758244756908140342ull
//   13760507618389236834ull -> 15566763870945589237ull
//   10358531465360738070ull -> 6873818787825873209ull
//   8303000664099948218ull -> 6812943286057894597ull
//   7402715195858175342ull -> 3387202600460903393ull
//   3035965499586260595ull -> 7466254715772248280ull
//   11719160029751101403ull -> 4787759272255391816ull
//   16206633647750508095ull -> 8616852669644928016ull
//   12855988691322821457ull -> 5169739093973667818ull
//   11120916648321494458ull -> 8114170972468563665ull
//   1893025614008146711ull -> 15451781156783790514ull
//   9547728811180246134ull -> 14928994079834474261ull
//   10584262505997145969ull -> 9000466801761918596ull
//   6103305640183140576ull -> 7218724214458732951ull
//   6345880683956586592ull -> 961957523390319947ull
//   2078476119103271494ull -> 6119612767569606339ull
//   16311671080874283707ull -> 6735844924784248912ull
//   3020759032456607957ull -> 14400968495830141814ull
//   1513315884295622487ull -> 10471799256157446552ull
//   2311351656297275347ull -> 11816074281630281704ull
//   17082747341248638856ull -> 12203639598286149491ull
//   12868112467043389558ull -> 13297688861346263409ull
//   1507001438527628604ull -> 7652020385574100696ull
//   2334120587823506820ull -> 13501287520271667536ull
//   547718020446064204ull -> 12693965216151243664ull
// expectation corrected (v19-E, kCancel_rows, 32 of 32 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   16509221787135873163ull -> 16885932052227463976ull
//   12162968985552207795ull -> 2527405596089513137ull
//   4269270195170776294ull -> 1474215827418029803ull
//   4853599967980404227ull -> 9280260176724097047ull
//   14437417135330127222ull -> 15321716256000571609ull
//   10586829955756583920ull -> 12073207399592924275ull
//   12081162804397252102ull -> 14164035305485858490ull
//   8758244756908140342ull -> 10545706318800751488ull
//   15566763870945589237ull -> 1775489816151474656ull
//   6873818787825873209ull -> 318726799888700949ull
//   6812943286057894597ull -> 1586484278605998308ull
//   3387202600460903393ull -> 11382308868487550244ull
//   7466254715772248280ull -> 10872772640051750680ull
//   4787759272255391816ull -> 4875115919163760185ull
//   8616852669644928016ull -> 7483740104829087331ull
//   5169739093973667818ull -> 1767653502614296251ull
//   8114170972468563665ull -> 3897954235023743852ull
//   15451781156783790514ull -> 3572309019897074723ull
//   14928994079834474261ull -> 6080227582324520130ull
//   9000466801761918596ull -> 16204570696603541185ull
//   7218724214458732951ull -> 8962979779500613143ull
//   961957523390319947ull -> 3890978689718480682ull
//   6119612767569606339ull -> 7128828949941915251ull
//   6735844924784248912ull -> 15641506033444665247ull
//   14400968495830141814ull -> 17384785312098846605ull
//   10471799256157446552ull -> 11912041959994493277ull
//   11816074281630281704ull -> 2066894531641232293ull
//   12203639598286149491ull -> 18138164710109019672ull
//   13297688861346263409ull -> 13099705273927855288ull
//   7652020385574100696ull -> 6317668264567744969ull
//   13501287520271667536ull -> 7015387317176510425ull
//   12693965216151243664ull -> 12078141763174815190ull
// expectation corrected (V19-FIX, kCancel_rows, 32 of 32 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   16885932052227463976ull -> 8933562200260968840ull
//   2527405596089513137ull -> 12907991694751175761ull
//   1474215827418029803ull -> 15492238351520247563ull
//   9280260176724097047ull -> 4949954141317803287ull
//   15321716256000571609ull -> 6869043455093337913ull
//   12073207399592924275ull -> 16700973384839330835ull
//   14164035305485858490ull -> 13907977122257783994ull
//   10545706318800751488ull -> 13690802804532944832ull
//   1775489816151474656ull -> 6641415404822534272ull
//   318726799888700949ull -> 17617700982422257653ull
//   1586484278605998308ull -> 8957395621418722884ull
//   11382308868487550244ull -> 15533748013016881700ull
//   10872772640051750680ull -> 10113927666003718936ull
//   4875115919163760185ull -> 10748025209891027257ull
//   7483740104829087331ull -> 7405357053482683011ull
//   1767653502614296251ull -> 14210999101428183707ull
//   3897954235023743852ull -> 5089886391592128332ull
//   3572309019897074723ull -> 1890214210532379459ull
//   6080227582324520130ull -> 15715071324930605218ull
//   16204570696603541185ull -> 12760333961958415553ull
//   8962979779500613143ull -> 1349906610605744215ull
//   3890978689718480682ull -> 11872699637369758186ull
//   7128828949941915251ull -> 6278706775858866003ull
//   15641506033444665247ull -> 8489616688255836383ull
//   17384785312098846605ull -> 8131243489711240877ull
//   11912041959994493277ull -> 73524659065323965ull
//   2066894531641232293ull -> 13120948974577109893ull
//   18138164710109019672ull -> 13036340438400450584ull
//   13099705273927855288ull -> 17320509982992261848ull
//   6317668264567744969ull -> 16001552197493914473ull
//   7015387317176510425ull -> 12375923388653592921ull
//   12078141763174815190ull -> 17283281126178883990ull
// expectation corrected (INT26 v19 hash re-pin, kCancel_rows, 32 of 32 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   8933562200260968840ull -> 8019365971493599225ull [H-THIN]
//   12907991694751175761ull -> 9755581588236959176ull [H-THIN]
//   15492238351520247563ull -> 8371403224338711542ull [H-THIN]
//   4949954141317803287ull -> 16042211801967482956ull [H-THIN]
//   6869043455093337913ull -> 16802932054993035276ull [H-THIN]
//   16700973384839330835ull -> 13204266665875046502ull [H-THIN]
//   13907977122257783994ull -> 16358615663034135167ull [H-THIN]
//   13690802804532944832ull -> 6541217908632789433ull [H-THIN]
//   6641415404822534272ull -> 16241388624320518101ull [H-THIN]
//   17617700982422257653ull -> 3812797098998110562ull [H-THIN]
//   8957395621418722884ull -> 18060365737908723675ull [H-THIN]
//   15533748013016881700ull -> 1812188758081721615ull [H-THIN]
//   10113927666003718936ull -> 5200369397060719481ull [H-THIN]
//   10748025209891027257ull -> 14528435239011413150ull [H-THIN]
//   7405357053482683011ull -> 16366047349482157266ull [H-THIN]
//   14210999101428183707ull -> 14462275783608418760ull [H-THIN]
//   5089886391592128332ull -> 15465277701067203145ull [H-THIN]
//   1890214210532379459ull -> 10822246540794760664ull [H-THIN]
//   15715071324930605218ull -> 5153708099721087119ull [H-THIN]
//   12760333961958415553ull -> 10351223234449594086ull [H-THIN]
//   1349906610605744215ull -> 17418701808057553558ull [H-THIN]
//   11872699637369758186ull -> 8964634856941185769ull [H-THIN]
//   6278706775858866003ull -> 8066351423005290858ull [H-THIN]
//   8489616688255836383ull -> 17339397096778631582ull [H-THIN]
//   8131243489711240877ull -> 10167217103912949158ull [H-THIN]
//   73524659065323965ull -> 7835551358700961936ull [H-THIN]
//   13120948974577109893ull -> 3057432646794745428ull [H-THIN]
//   13036340438400450584ull -> 3633706464540692363ull [H-THIN]
//   17320509982992261848ull -> 17115428998086447629ull [H-THIN]
//   16001552197493914473ull -> 6180321870109069012ull [H-THIN]
//   12375923388653592921ull -> 13623488446145227464ull [H-THIN]
//   17283281126178883990ull -> 7433824449803764975ull [H-THIN]
constexpr std::uint64_t kCancel_rows[] = {
    8019365971493599225ull,
    9755581588236959176ull,
    8371403224338711542ull,
    16042211801967482956ull,
    16802932054993035276ull,
    13204266665875046502ull,
    16358615663034135167ull,
    6541217908632789433ull,
    16241388624320518101ull,
    3812797098998110562ull,
    18060365737908723675ull,
    1812188758081721615ull,
    5200369397060719481ull,
    14528435239011413150ull,
    16366047349482157266ull,
    14462275783608418760ull,
    15465277701067203145ull,
    10822246540794760664ull,
    5153708099721087119ull,
    10351223234449594086ull,
    17418701808057553558ull,
    8964634856941185769ull,
    8066351423005290858ull,
    17339397096778631582ull,
    10167217103912949158ull,
    7835551358700961936ull,
    3057432646794745428ull,
    3633706464540692363ull,
    17115428998086447629ull,
    6180321870109069012ull,
    13623488446145227464ull,
    7433824449803764975ull,
};
// expectation corrected (kCancel_reads, 19 of 19 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   11036775732707302117ull -> 4251966616960422708ull
//   12173319204132133167ull -> 14959224797235029426ull
//   15089384555745740641ull -> 18118854639115735744ull
//   12657050168316361590ull -> 11588712661185214465ull
//   5508522320031760046ull -> 4733682306657357913ull
//   4392280703809030577ull -> 4867538669402761990ull
//   5672602513915569466ull -> 9045222791752902197ull
//   1584111208947804940ull -> 9067150155065020095ull
//   17604116988579061482ull -> 16582276596475662221ull
//   2571643557053314723ull -> 961163065206318252ull
//   12263557600672438644ull -> 12708349385405222195ull
//   17566757678734830290ull -> 12224111853153826757ull
//   14239558782549945156ull -> 18058899645842030845ull
//   210949755977273831ull -> 13301152032237995172ull
//   12332984203645592185ull -> 18445655867616170140ull
//   2007155456688924841ull -> 16801366699711482094ull
//   3006055157799811198ull -> 16248277367358811299ull
//   8285142442694716882ull -> 8861077440513858845ull
//   14587662939274775057ull -> 6769299827481558054ull
// expectation corrected (v19-E, kCancel_reads, 19 of 19 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   4251966616960422708ull -> 14973544158008760732ull
//   14959224797235029426ull -> 11798837567516325992ull
//   18118854639115735744ull -> 12592890165665480327ull
//   11588712661185214465ull -> 12442547841905437363ull
//   4733682306657357913ull -> 4200291776743362805ull
//   4867538669402761990ull -> 6978595633330587854ull
//   9045222791752902197ull -> 4620048550389167469ull
//   9067150155065020095ull -> 4486444215607032147ull
//   16582276596475662221ull -> 14610824379851499445ull
//   961163065206318252ull -> 13884124081678429497ull
//   12708349385405222195ull -> 8996981246421058099ull
//   12224111853153826757ull -> 16696428861158375881ull
//   18058899645842030845ull -> 7037104328891838191ull
//   13301152032237995172ull -> 12474725854359419609ull
//   18445655867616170140ull -> 14933168323669631219ull
//   16801366699711482094ull -> 13091180856139824017ull
//   16248277367358811299ull -> 332263738452834604ull
//   8861077440513858845ull -> 11019138486722009680ull
//   6769299827481558054ull -> 1811755191633478047ull
// expectation corrected (V19-FIX, kCancel_reads, 19 of 19 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   14973544158008760732ull -> 5066383747043332732ull
//   11798837567516325992ull -> 14642164039413495816ull
//   12592890165665480327ull -> 1826995519720684135ull
//   12442547841905437363ull -> 10600767793424807315ull
//   4200291776743362805ull -> 18413531801076714261ull
//   6978595633330587854ull -> 7162595064832704878ull
//   4620048550389167469ull -> 8771487694918498925ull
//   4486444215607032147ull -> 3727599241559000403ull
//   14610824379851499445ull -> 2036989596869214901ull
//   13884124081678429497ull -> 15857207205534857561ull
//   8996981246421058099ull -> 5509984618602967315ull
//   16696428861158375881ull -> 227260307711721321ull
//   7037104328891838191ull -> 14543777879962894095ull
//   12474725854359419609ull -> 2123158589903514105ull
//   14933168323669631219ull -> 11488931589024505587ull
//   13091180856139824017ull -> 6035902334985171665ull
//   332263738452834604ull -> 6330997617635358220ull
//   11019138486722009680ull -> 7622013860668800144ull
//   1811755191633478047ull -> 7135414897549443327ull
// expectation corrected (INT26 v19 hash re-pin, kCancel_reads, 19 of 19 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   5066383747043332732ull -> 16017138806894645973ull [H-THIN]
//   14642164039413495816ull -> 14395189169447138845ull [H-THIN]
//   1826995519720684135ull -> 15433550368465758174ull [H-THIN]
//   10600767793424807315ull -> 15319181961208510706ull [H-THIN]
//   18413531801076714261ull -> 7116101502612005176ull [H-THIN]
//   7162595064832704878ull -> 5624598517067648741ull [H-THIN]
//   8771487694918498925ull -> 15626337877376063418ull [H-THIN]
//   3727599241559000403ull -> 12303175751506120794ull [H-THIN]
//   2036989596869214901ull -> 16768996190976851778ull [H-THIN]
//   15857207205534857561ull -> 9166644978234881348ull [H-THIN]
//   5509984618602967315ull -> 18107466278445992216ull [H-THIN]
//   227260307711721321ull -> 512994913100055200ull [H-THIN]
//   14543777879962894095ull -> 9443988914314833904ull [H-THIN]
//   2123158589903514105ull -> 15301609864008799700ull [H-THIN]
//   11488931589024505587ull -> 18376839094291697164ull [H-THIN]
//   6035902334985171665ull -> 3352239734550209384ull [H-THIN]
//   6330997617635358220ull -> 9556582455608271345ull [H-THIN]
//   7622013860668800144ull -> 4597279093679219283ull [H-THIN]
//   7135414897549443327ull -> 12371316346718768834ull [H-THIN]
constexpr std::uint64_t kCancel_reads[] = {
    16017138806894645973ull,
    14395189169447138845ull,
    15433550368465758174ull,
    15319181961208510706ull,
    7116101502612005176ull,
    5624598517067648741ull,
    15626337877376063418ull,
    12303175751506120794ull,
    16768996190976851778ull,
    9166644978234881348ull,
    18107466278445992216ull,
    512994913100055200ull,
    9443988914314833904ull,
    15301609864008799700ull,
    18376839094291697164ull,
    3352239734550209384ull,
    9556582455608271345ull,
    4597279093679219283ull,
    12371316346718768834ull,
};
// expectation corrected: kCancel_final 547718020446064204ull -> 12693965216151243664ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move.
// expectation corrected (v19-E): kCancel_final 12693965216151243664ull -> 12078141763174815190ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
// expectation corrected (V19-FIX): kCancel_final 12078141763174815190ull -> 17283281126178883990ull, because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree.
// expectation corrected (INT26 v19 hash re-pin): kCancel_final 17283281126178883990ull -> 7433824449803764975ull [H-THIN], because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move.
constexpr std::uint64_t kCancel_final = 7433824449803764975ull;
constexpr Trade kCancel_trades[] = {
    {1736122980000LL, 1736123340000LL, 101, 102, 2, 0},
};

// expectation corrected (kOca_rows, 32 of 32 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   16709085573648202138ull -> 16509221787135873163ull
//   15515119955049707023ull -> 16213424625920948778ull
//   2592424843452700379ull -> 17872044691828503814ull
//   16683291785383231136ull -> 15919755911706481749ull
//   17199212447046980747ull -> 11977823608413303746ull
//   2771767984845509636ull -> 2039595034277456393ull
//   9309510232087592598ull -> 10104456447379069385ull
//   15960215247508421371ull -> 5643206015427102130ull
//   7759688160476602112ull -> 2506220035595736261ull
//   17187218502565685484ull -> 10918296732737538893ull
//   6559742754627284760ull -> 3189275971136133817ull
//   18118180980177972209ull -> 6429214289272271872ull
//   13215094269827373432ull -> 15960194113053071985ull
//   13923831569574677590ull -> 5760740312474779575ull
//   7563273161250924677ull -> 12140601788445559132ull
//   5322278216926092479ull -> 6775104474122977788ull
//   16869350303802269268ull -> 15093957348750749987ull
//   16954816832214052155ull -> 9471505608977096386ull
//   3126570409416861252ull -> 7039923310276819473ull
//   2996293251300168498ull -> 10214777438842844527ull
//   9588882328803106067ull -> 2297687339521714238ull
//   9262140034190745255ull -> 9819741921439305378ull
//   16130298844759027972ull -> 8893920735129372673ull
//   15712758552780957086ull -> 1202447636247627974ull
//   12134094289949509338ull -> 6855292384500989378ull
//   14560200042856682656ull -> 15975703766269642304ull
//   17519121608160305268ull -> 17508022296314285140ull
//   16099910215857864057ull -> 16006929479311048729ull
//   7391386743554248102ull -> 4403170830343122886ull
//   18021125781548550855ull -> 4219282542331499751ull
//   16440759220253120341ull -> 16213827754665597595ull
//   13344165461917220241ull -> 14677771083385306600ull
// expectation corrected (v19-E, kOca_rows, 32 of 32 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   16509221787135873163ull -> 16885932052227463976ull
//   16213424625920948778ull -> 1764667717626824743ull
//   17872044691828503814ull -> 4322727655245959010ull
//   15919755911706481749ull -> 1395244849641228558ull
//   11977823608413303746ull -> 2140358424951924404ull
//   2039595034277456393ull -> 9699116680553236170ull
//   10104456447379069385ull -> 7946281078129361061ull
//   5643206015427102130ull -> 7217030725170179233ull
//   2506220035595736261ull -> 13130200612041399664ull
//   10918296732737538893ull -> 5287199831653622788ull
//   3189275971136133817ull -> 3506631918949547901ull
//   6429214289272271872ull -> 4465527976539731669ull
//   15960194113053071985ull -> 5688012065610201252ull
//   5760740312474779575ull -> 15517873125024367971ull
//   12140601788445559132ull -> 5873090822233458790ull
//   6775104474122977788ull -> 8742784406691043605ull
//   15093957348750749987ull -> 3606250785412036839ull
//   9471505608977096386ull -> 8169195814552631507ull
//   7039923310276819473ull -> 14272420827081783476ull
//   10214777438842844527ull -> 16588921592304671538ull
//   2297687339521714238ull -> 3210670836326271236ull
//   9819741921439305378ull -> 2166897132651157340ull
//   8893920735129372673ull -> 8856912385722987180ull
//   1202447636247627974ull -> 8380781416426648794ull
//   6855292384500989378ull -> 6191340745574236276ull
//   15975703766269642304ull -> 6226269427383285383ull
//   17508022296314285140ull -> 15465297086043070479ull
//   16006929479311048729ull -> 10592838278007696860ull
//   4403170830343122886ull -> 15117205657257447887ull
//   4219282542331499751ull -> 312036520819661464ull
//   16213827754665597595ull -> 15975581040180058350ull
//   14677771083385306600ull -> 15465526605242472570ull
// expectation corrected (V19-FIX, kOca_rows, 32 of 32 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   16885932052227463976ull -> 8933562200260968840ull
//   1764667717626824743ull -> 16488569157039504391ull
//   4322727655245959010ull -> 15308668032647221442ull
//   1395244849641228558ull -> 11025020666800289870ull
//   2140358424951924404ull -> 17376859899643293076ull
//   9699116680553236170ull -> 18031995174781967338ull
//   7946281078129361061ull -> 9135991105341529701ull
//   7217030725170179233ull -> 12521501026388244289ull
//   13130200612041399664ull -> 17387999442892602416ull
//   5287199831653622788ull -> 17523447275006301316ull
//   3506631918949547901ull -> 700544763663270973ull
//   4465527976539731669ull -> 6419555011776275829ull
//   5688012065610201252ull -> 9501522493702417732ull
//   15517873125024367971ull -> 2198037920276336707ull
//   5873090822233458790ull -> 3899965171878021222ull
//   8742784406691043605ull -> 17400108061693215989ull
//   3606250785412036839ull -> 6268695127500094535ull
//   8169195814552631507ull -> 15386469441856039635ull
//   14272420827081783476ull -> 1827308857775093108ull
//   16588921592304671538ull -> 9002315039531479122ull
//   3210670836326271236ull -> 4336820592134659556ull
//   2166897132651157340ull -> 1568941454279137340ull
//   8856912385722987180ull -> 3586528324041147724ull
//   8380781416426648794ull -> 9921466732535290906ull
//   6191340745574236276ull -> 2210251816763789972ull
//   6226269427383285383ull -> 12540125623729258183ull
//   15465297086043070479ull -> 4922139678029842511ull
//   10592838278007696860ull -> 14880777043208119420ull
//   15117205657257447887ull -> 766085567512147215ull
//   312036520819661464ull -> 12667708910662510744ull
//   15975581040180058350ull -> 17116188563837257134ull
//   15465526605242472570ull -> 18394579596403092954ull
// expectation corrected (INT26 v19 hash re-pin, kOca_rows, 32 of 32 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   8933562200260968840ull -> 8019365971493599225ull [H-THIN]
//   16488569157039504391ull -> 3232936220238691146ull [H-THIN]
//   15308668032647221442ull -> 8093402965097552867ull [H-THIN]
//   11025020666800289870ull -> 16342776694917511399ull [H-THIN]
//   17376859899643293076ull -> 1176409931075103109ull [H-THIN]
//   18031995174781967338ull -> 14796994139400300899ull [H-THIN]
//   9135991105341529701ull -> 4041329064345231230ull [H-THIN]
//   12521501026388244289ull -> 18203940660042607242ull [H-THIN]
//   17387999442892602416ull -> 11789920335390250025ull [H-THIN]
//   17523447275006301316ull -> 12162719525992402073ull [H-THIN]
//   700544763663270973ull -> 10376658214325705912ull [H-THIN]
//   6419555011776275829ull -> 985387310022141304ull [H-THIN]
//   9501522493702417732ull -> 11563487340980545165ull [H-THIN]
//   2198037920276336707ull -> 17750573796976151050ull [H-THIN]
//   3899965171878021222ull -> 8782861391928203675ull [H-THIN]
//   17400108061693215989ull -> 148042859240194154ull [H-THIN]
//   6268695127500094535ull -> 2639689295090791304ull [H-THIN]
//   15386469441856039635ull -> 10798952018967388820ull [H-THIN]
//   1827308857775093108ull -> 13015585966305206737ull [H-THIN]
//   9002315039531479122ull -> 619093614529828615ull [H-THIN]
//   4336820592134659556ull -> 16432223468538835037ull [H-THIN]
//   1568941454279137340ull -> 5709266478406133937ull [H-THIN]
//   3586528324041147724ull -> 11031153128114419525ull [H-THIN]
//   9921466732535290906ull -> 17759043185727148801ull [H-THIN]
//   2210251816763789972ull -> 778153452226410841ull [H-THIN]
//   12540125623729258183ull -> 6929057138192640130ull [H-THIN]
//   4922139678029842511ull -> 1014477579839442850ull [H-THIN]
//   14880777043208119420ull -> 14398065235087572465ull [H-THIN]
//   766085567512147215ull -> 10116121127485637382ull [H-THIN]
//   12667708910662510744ull -> 9104659107751229673ull [H-THIN]
//   17116188563837257134ull -> 12444616329691358185ull [H-THIN]
//   18394579596403092954ull -> 14761567357674824267ull [H-THIN]
constexpr std::uint64_t kOca_rows[] = {
    8019365971493599225ull,
    3232936220238691146ull,
    8093402965097552867ull,
    16342776694917511399ull,
    1176409931075103109ull,
    14796994139400300899ull,
    4041329064345231230ull,
    18203940660042607242ull,
    11789920335390250025ull,
    12162719525992402073ull,
    10376658214325705912ull,
    985387310022141304ull,
    11563487340980545165ull,
    17750573796976151050ull,
    8782861391928203675ull,
    148042859240194154ull,
    2639689295090791304ull,
    10798952018967388820ull,
    13015585966305206737ull,
    619093614529828615ull,
    16432223468538835037ull,
    5709266478406133937ull,
    11031153128114419525ull,
    17759043185727148801ull,
    778153452226410841ull,
    6929057138192640130ull,
    1014477579839442850ull,
    14398065235087572465ull,
    10116121127485637382ull,
    9104659107751229673ull,
    12444616329691358185ull,
    14761567357674824267ull,
};
// expectation corrected (kOca_reads, 12 of 12 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   14283998503469032055ull -> 10089579219758468484ull
//   1440851436719442360ull -> 2072661251013514801ull
//   13628616919967222108ull -> 11806859925838030599ull
//   3250156598192879549ull -> 3859832355603682894ull
//   5694049929555431913ull -> 3781711652309471012ull
//   8532241677982258469ull -> 13458268170875603960ull
//   10905738441214044975ull -> 17505340266458019922ull
//   5471479283459854020ull -> 9166139179025562633ull
//   13881602945166894983ull -> 13617685697785145770ull
//   922624683464432553ull -> 18038646180534657621ull
//   8476970287391158316ull -> 7648413869989003340ull
//   11307620409506918497ull -> 17778673258699498459ull
// expectation corrected (v19-E, kOca_reads, 12 of 12 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   10089579219758468484ull -> 5684519712303414667ull
//   2072661251013514801ull -> 15423667651443634118ull
//   11806859925838030599ull -> 4216971117943841170ull
//   3859832355603682894ull -> 1090467243908742462ull
//   3781711652309471012ull -> 9157803620682564923ull
//   13458268170875603960ull -> 10534790258471791480ull
//   17505340266458019922ull -> 11014335311344812052ull
//   9166139179025562633ull -> 14978113568142998395ull
//   13617685697785145770ull -> 10741037411738045890ull
//   18038646180534657621ull -> 12870434045680047141ull
//   7648413869989003340ull -> 5966302497310935881ull
//   17778673258699498459ull -> 5749065519581012093ull
// expectation corrected (V19-FIX, kOca_reads, 12 of 12 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   5684519712303414667ull -> 3105752370505168555ull
//   15423667651443634118ull -> 13318576850138903270ull
//   4216971117943841170ull -> 4943429593008048082ull
//   1090467243908742462ull -> 7900987300136689534ull
//   9157803620682564923ull -> 16889527581053605563ull
//   10534790258471791480ull -> 8561664608116353912ull
//   11014335311344812052ull -> 10377670066927504148ull
//   14978113568142998395ull -> 14341448323725690491ull
//   10741037411738045890ull -> 9080486838152177794ull
//   12870434045680047141ull -> 3016108360062111013ull
//   5966302497310935881ull -> 14134368882450491401ull
//   5749065519581012093ull -> 12755516166577050941ull
// expectation corrected (INT26 v19 hash re-pin, kOca_reads, 12 of 12 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   3105752370505168555ull -> 15189525287878030606ull [H-THIN]
//   13318576850138903270ull -> 12039483643790541963ull [H-THIN]
//   4943429593008048082ull -> 16889317257125145965ull [H-THIN]
//   7900987300136689534ull -> 14061180146549677313ull [H-THIN]
//   16889527581053605563ull -> 3457726981161275090ull [H-THIN]
//   8561664608116353912ull -> 15838028967223682041ull [H-THIN]
//   10377670066927504148ull -> 11019583341156392335ull [H-THIN]
//   14341448323725690491ull -> 15037407705806185768ull [H-THIN]
//   9080486838152177794ull -> 17212813844302948315ull [H-THIN]
//   3016108360062111013ull -> 8321492686618048122ull [H-THIN]
//   14134368882450491401ull -> 12537836709650543420ull [H-THIN]
//   12755516166577050941ull -> 5737128231549608782ull [H-THIN]
constexpr std::uint64_t kOca_reads[] = {
    15189525287878030606ull,
    12039483643790541963ull,
    16889317257125145965ull,
    14061180146549677313ull,
    3457726981161275090ull,
    15838028967223682041ull,
    11019583341156392335ull,
    15037407705806185768ull,
    17212813844302948315ull,
    8321492686618048122ull,
    12537836709650543420ull,
    5737128231549608782ull,
};
// expectation corrected: kOca_final 13344165461917220241ull -> 14677771083385306600ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move.
// expectation corrected (v19-E): kOca_final 14677771083385306600ull -> 15465526605242472570ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
// expectation corrected (V19-FIX): kOca_final 15465526605242472570ull -> 18394579596403092954ull, because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree.
// expectation corrected (INT26 v19 hash re-pin): kOca_final 18394579596403092954ull -> 14761567357674824267ull [H-THIN], because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move.
constexpr std::uint64_t kOca_final = 14761567357674824267ull;
constexpr Trade kOca_trades[] = {
    {1736121960000LL, 1736122020000LL, 100.75, 100.75, 2, 0},
    {1736122500000LL, 1736122980000LL, 100.5, 101, 2, 0},
    {1736122680000LL, 1736122980000LL, 101, 101, 2, 0},
    {1736123400000LL, 1736123460000LL, 100.75, 100.75, 2, 0},
};

// expectation corrected (kReissue_rows, 32 of 32 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   8585548509716714081ull -> 18024613014852726460ull
//   14325204492038295877ull -> 1074213683018110050ull
//   11333416975755677149ull -> 13147902176710415630ull
//   16543411225283864648ull -> 13793724757770315635ull
//   10938205834961117430ull -> 3863351525150972485ull
//   11387688170653017850ull -> 10045367816869839457ull
//   15953473043350382429ull -> 6286850115029714034ull
//   6572487799333293791ull -> 7747547030589476316ull
//   6528665896004934324ull -> 1924532075938187007ull
//   15865689254201632410ull -> 3686371609234220229ull
//   13278955049226971361ull -> 14587432707151841222ull
//   6953659889899964851ull -> 16753939143680858740ull
//   17379697670695539997ull -> 18393033864700417738ull
//   17872590505934490011ull -> 13412749746999424712ull
//   5749269872046101394ull -> 2010106352901326505ull
//   18391395619855151097ull -> 3288159458834122842ull
//   15052170923686004435ull -> 3658558269269208344ull
//   8530654778004631357ull -> 3950966298662106526ull
//   5149815064334227318ull -> 1130949384479819001ull
//   14245712945540619235ull -> 17814833468986496020ull
//   10733703939886245336ull -> 16368933599130155435ull
//   6273466604925818965ull -> 15086902738405765562ull
//   4079488818521470261ull -> 6851544227833672694ull
//   2146385853437671914ull -> 9983206406500927537ull
//   15888322699235513727ull -> 2903402778481137828ull
//   11387314208000124801ull -> 3205506498807496032ull
//   13309321548283340533ull -> 71458747074842696ull
//   11569906020034529205ull -> 17696525960374254448ull
//   7391169599327399216ull -> 3134152725610456209ull
//   17943993331344782879ull -> 13204409897368598050ull
//   8161091785660856871ull -> 5202308151562800438ull
//   10597617675334596827ull -> 5214787630894246342ull
// expectation corrected (v19-E, kReissue_rows, 32 of 32 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   18024613014852726460ull -> 10502385223359640218ull
//   1074213683018110050ull -> 16623997955197906952ull
//   13147902176710415630ull -> 811252746116859749ull
//   13793724757770315635ull -> 10758794706905446193ull
//   3863351525150972485ull -> 15617716314233334546ull
//   10045367816869839457ull -> 6767772143179858797ull
//   6286850115029714034ull -> 5521983648121747859ull
//   7747547030589476316ull -> 15617777410714397671ull
//   1924532075938187007ull -> 18135165643667734470ull
//   3686371609234220229ull -> 7866918066084324772ull
//   14587432707151841222ull -> 13083169176416003415ull
//   16753939143680858740ull -> 975673565472503276ull
//   18393033864700417738ull -> 3561629021139748812ull
//   13412749746999424712ull -> 2811479979004047652ull
//   2010106352901326505ull -> 16463592783525505269ull
//   3288159458834122842ull -> 12265787473190619113ull
//   3658558269269208344ull -> 12140961588712596768ull
//   3950966298662106526ull -> 10187321206192027662ull
//   1130949384479819001ull -> 3504288509090403179ull
//   17814833468986496020ull -> 15037639390730560913ull
//   16368933599130155435ull -> 7303096138953671912ull
//   15086902738405765562ull -> 9693853816007893072ull
//   6851544227833672694ull -> 8169735607747040047ull
//   9983206406500927537ull -> 2591689537386467449ull
//   2903402778481137828ull -> 14207231819871013441ull
//   3205506498807496032ull -> 5330583192549066300ull
//   71458747074842696ull -> 13611713462682254439ull
//   17696525960374254448ull -> 10373123901283573478ull
//   3134152725610456209ull -> 16143045497135443280ull
//   13204409897368598050ull -> 13079457144551358245ull
//   5202308151562800438ull -> 10683567794089412281ull
//   5214787630894246342ull -> 14699793179829041832ull
// expectation corrected (V19-FIX, kReissue_rows, 32 of 32 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   10502385223359640218ull -> 11371589716169913082ull
//   16623997955197906952ull -> 11912421295804525448ull
//   811252746116859749ull -> 16162904808422149189ull
//   10758794706905446193ull -> 1884050390405525809ull
//   15617716314233334546ull -> 1630941943368694258ull
//   6767772143179858797ull -> 1646315788396378413ull
//   5521983648121747859ull -> 3202536020749475123ull
//   15617777410714397671ull -> 16739146824408715591ull
//   18135165643667734470ull -> 12465965272475823462ull
//   7866918066084324772ull -> 12416019024705234276ull
//   13083169176416003415ull -> 8019931705879186711ull
//   975673565472503276ull -> 10511748891040354060ull
//   3561629021139748812ull -> 7873647981647748428ull
//   2811479979004047652ull -> 13583506357494126788ull
//   16463592783525505269ull -> 6827456748487052757ull
//   12265787473190619113ull -> 4860432548138722281ull
//   12140961588712596768ull -> 10318257384229209728ull
//   10187321206192027662ull -> 13270746490627165070ull
//   3504288509090403179ull -> 15647754736958033867ull
//   15037639390730560913ull -> 15257413490644512113ull
//   7303096138953671912ull -> 3422115173668721064ull
//   9693853816007893072ull -> 11954333827210587376ull
//   8169735607747040047ull -> 16717809947662223375ull
//   2591689537386467449ull -> 16431286081784348505ull
//   14207231819871013441ull -> 14674774925924029409ull
//   5330583192549066300ull -> 8888789072620710620ull
//   13611713462682254439ull -> 7677274857978420999ull
//   10373123901283573478ull -> 8389711543599766598ull
//   16143045497135443280ull -> 11866583660361571056ull
//   13079457144551358245ull -> 8782474388336378853ull
//   10683567794089412281ull -> 12830062157342008889ull
//   14699793179829041832ull -> 11660101618206053704ull
// expectation corrected (INT26 v19 hash re-pin, kReissue_rows, 32 of 32 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   11371589716169913082ull -> 5794085833535179433ull [H-THIN]
//   11912421295804525448ull -> 10206102349587901947ull [H-THIN]
//   16162904808422149189ull -> 16740711338258568218ull [H-THIN]
//   1884050390405525809ull -> 157145938453825824ull [H-THIN]
//   1630941943368694258ull -> 11549520461694658189ull [H-THIN]
//   1646315788396378413ull -> 375980444520009614ull [H-THIN]
//   3202536020749475123ull -> 1364356711154384662ull [H-THIN]
//   16739146824408715591ull -> 9267951354912006062ull [H-THIN]
//   12465965272475823462ull -> 18013247113906258621ull [H-THIN]
//   12416019024705234276ull -> 15135278226663906553ull [H-THIN]
//   8019931705879186711ull -> 6395419845332150882ull [H-THIN]
//   10511748891040354060ull -> 2608642583221907887ull [H-THIN]
//   7873647981647748428ull -> 1439014296977039245ull [H-THIN]
//   13583506357494126788ull -> 4897379921696966153ull [H-THIN]
//   6827456748487052757ull -> 13502194017489163244ull [H-THIN]
//   4860432548138722281ull -> 8254807225619272358ull [H-THIN]
//   10318257384229209728ull -> 4041173354564745731ull [H-THIN]
//   13270746490627165070ull -> 5823046275575919511ull [H-THIN]
//   15647754736958033867ull -> 11150667084723241358ull [H-THIN]
//   15257413490644512113ull -> 7066649565605271382ull [H-THIN]
//   3422115173668721064ull -> 9147117666804548605ull [H-THIN]
//   11954333827210587376ull -> 2943969262508479583ull [H-THIN]
//   16717809947662223375ull -> 2242050686395926078ull [H-THIN]
//   16431286081784348505ull -> 18297932585058907168ull [H-THIN]
//   14674774925924029409ull -> 7328278135585233712ull [H-THIN]
//   8888789072620710620ull -> 4496911868369281609ull [H-THIN]
//   7677274857978420999ull -> 4092312770444862044ull [H-THIN]
//   8389711543599766598ull -> 3103138085614071329ull [H-THIN]
//   11866583660361571056ull -> 1640897772442143723ull [H-THIN]
//   8782474388336378853ull -> 11067585398331222410ull [H-THIN]
//   12830062157342008889ull -> 358041756474409844ull [H-THIN]
//   11660101618206053704ull -> 8735900797794446825ull [H-THIN]
constexpr std::uint64_t kReissue_rows[] = {
    5794085833535179433ull,
    10206102349587901947ull,
    16740711338258568218ull,
    157145938453825824ull,
    11549520461694658189ull,
    375980444520009614ull,
    1364356711154384662ull,
    9267951354912006062ull,
    18013247113906258621ull,
    15135278226663906553ull,
    6395419845332150882ull,
    2608642583221907887ull,
    1439014296977039245ull,
    4897379921696966153ull,
    13502194017489163244ull,
    8254807225619272358ull,
    4041173354564745731ull,
    5823046275575919511ull,
    11150667084723241358ull,
    7066649565605271382ull,
    9147117666804548605ull,
    2943969262508479583ull,
    2242050686395926078ull,
    18297932585058907168ull,
    7328278135585233712ull,
    4496911868369281609ull,
    4092312770444862044ull,
    3103138085614071329ull,
    1640897772442143723ull,
    11067585398331222410ull,
    358041756474409844ull,
    8735900797794446825ull,
};
// expectation corrected (kReissue_reads, 22 of 22 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   3939475029491512578ull -> 12411711249896042591ull
//   7356730424947321612ull -> 15648059662609975591ull
//   4328265905674352487ull -> 5984977296768921708ull
//   15527784777326893165ull -> 4966357655274817990ull
//   9073698856330894556ull -> 17598712414683929135ull
//   2843133185829080314ull -> 3385036857877132685ull
//   18285520585784536762ull -> 16336799525682361237ull
//   3605867739876178206ull -> 15900689561764743693ull
//   11617385860051029808ull -> 7628081347183665543ull
//   6120070253966533302ull -> 12667743022684261305ull
//   12884089841196503522ull -> 16636485711423056341ull
//   1757057082581109267ull -> 4211104103272306796ull
//   15144596411273886687ull -> 9074564720538305196ull
//   16491034907638174255ull -> 9054625658820012924ull
//   17587854081778798790ull -> 14276052809477962133ull
//   7020653761431304951ull -> 7882037541995811570ull
//   6011692623971064780ull -> 16285175368748575105ull
//   16619768330427881043ull -> 4622705585032064210ull
//   15988479060284894684ull -> 1279865497295564013ull
//   16104785410922037962ull -> 17779211270428359923ull
//   9473156753073087859ull -> 13487320763960909382ull
//   12580998165444248416ull -> 3448145313609564269ull
// expectation corrected (v19-E, kReissue_reads, 22 of 22 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   12411711249896042591ull -> 14558537749344697025ull
//   15648059662609975591ull -> 12590785953099354599ull
//   5984977296768921708ull -> 17377990171183053043ull
//   4966357655274817990ull -> 16572248048415135888ull
//   17598712414683929135ull -> 7437751090472884240ull
//   3385036857877132685ull -> 1419423412163814619ull
//   16336799525682361237ull -> 7256904348220848392ull
//   15900689561764743693ull -> 7147605502070139471ull
//   7628081347183665543ull -> 15244420174526167777ull
//   12667743022684261305ull -> 4351182266551369470ull
//   16636485711423056341ull -> 11321314328408210670ull
//   4211104103272306796ull -> 16652192784271152720ull
//   9074564720538305196ull -> 2529807158326617239ull
//   9054625658820012924ull -> 8285441707971576996ull
//   14276052809477962133ull -> 7222939095386434042ull
//   7882037541995811570ull -> 8143676236938658752ull
//   16285175368748575105ull -> 10250074498143150374ull
//   4622705585032064210ull -> 15356263522172424512ull
//   1279865497295564013ull -> 5156781438342475486ull
//   17779211270428359923ull -> 12085354800256371026ull
//   13487320763960909382ull -> 13244348858005104150ull
//   3448145313609564269ull -> 1758314424234447340ull
// expectation corrected (V19-FIX, kReissue_reads, 22 of 22 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   14558537749344697025ull -> 9397758857253294369ull
//   12590785953099354599ull -> 14952922796737447431ull
//   17377990171183053043ull -> 350142526787915315ull
//   16572248048415135888ull -> 9287618455889763760ull
//   7437751090472884240ull -> 8049051401679342672ull
//   1419423412163814619ull -> 7334056035772970331ull
//   7256904348220848392ull -> 16777030856659722344ull
//   7147605502070139471ull -> 12861194332908904879ull
//   15244420174526167777ull -> 7839065249474270945ull
//   4351182266551369470ull -> 2385401613699651006ull
//   11321314328408210670ull -> 3151001040546167918ull
//   16652192784271152720ull -> 6768266663339224688ull
//   2529807158326617239ull -> 211423388548179255ull
//   8285441707971576996ull -> 10932414881536382884ull
//   7222939095386434042ull -> 15509429580401113050ull
//   8143676236938658752ull -> 272542696402235168ull
//   10250074498143150374ull -> 12650608718358639686ull
//   15356263522172424512ull -> 16954864996066484000ull
//   5156781438342475486ull -> 2072712234614561342ull
//   12085354800256371026ull -> 15829080338314944658ull
//   13244348858005104150ull -> 2272828286921366934ull
//   1758314424234447340ull -> 16041091954465179468ull
// expectation corrected (INT26 v19 hash re-pin, kReissue_reads, 22 of 22 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   9397758857253294369ull -> 15035498073180564574ull [H-THIN]
//   14952922796737447431ull -> 6351174026430191720ull [H-THIN]
//   350142526787915315ull -> 17029808830897663162ull [H-THIN]
//   9287618455889763760ull -> 1960056261526077029ull [H-THIN]
//   8049051401679342672ull -> 9359327854260104443ull [H-THIN]
//   7334056035772970331ull -> 9193727488393939674ull [H-THIN]
//   16777030856659722344ull -> 2562235961360585845ull [H-THIN]
//   12861194332908904879ull -> 15091373470769183170ull [H-THIN]
//   7839065249474270945ull -> 9066532592892510086ull [H-THIN]
//   2385401613699651006ull -> 15993610558080257077ull [H-THIN]
//   3151001040546167918ull -> 14590383768983463647ull [H-THIN]
//   6768266663339224688ull -> 14847391113272722757ull [H-THIN]
//   211423388548179255ull -> 9991682699480081944ull [H-THIN]
//   10932414881536382884ull -> 7606589495236393781ull [H-THIN]
//   15509429580401113050ull -> 16170401377191376471ull [H-THIN]
//   272542696402235168ull -> 8660956339548506257ull [H-THIN]
//   12650608718358639686ull -> 17474680937306256469ull [H-THIN]
//   16954864996066484000ull -> 14583099152188767967ull [H-THIN]
//   2072712234614561342ull -> 1043496941119894989ull [H-THIN]
//   15829080338314944658ull -> 13855326176576914141ull [H-THIN]
//   2272828286921366934ull -> 10342641769074306343ull [H-THIN]
//   16041091954465179468ull -> 11480238464600989237ull [H-THIN]
constexpr std::uint64_t kReissue_reads[] = {
    15035498073180564574ull,
    6351174026430191720ull,
    17029808830897663162ull,
    1960056261526077029ull,
    9359327854260104443ull,
    9193727488393939674ull,
    2562235961360585845ull,
    15091373470769183170ull,
    9066532592892510086ull,
    15993610558080257077ull,
    14590383768983463647ull,
    14847391113272722757ull,
    9991682699480081944ull,
    7606589495236393781ull,
    16170401377191376471ull,
    8660956339548506257ull,
    17474680937306256469ull,
    14583099152188767967ull,
    1043496941119894989ull,
    13855326176576914141ull,
    10342641769074306343ull,
    11480238464600989237ull,
};
// expectation corrected: kReissue_final 10597617675334596827ull -> 5214787630894246342ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move.
// expectation corrected (v19-E): kReissue_final 5214787630894246342ull -> 14699793179829041832ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
// expectation corrected (V19-FIX): kReissue_final 14699793179829041832ull -> 11660101618206053704ull, because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree.
// expectation corrected (INT26 v19 hash re-pin): kReissue_final 11660101618206053704ull -> 8735900797794446825ull [H-THIN], because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move.
constexpr std::uint64_t kReissue_final = 8735900797794446825ull;
constexpr Trade kReissue_trades[] = {
    {1736121660000LL, 1736123040000LL, 100.75, 99.75, 2, 0},
    {1736123100000LL, 1736123460000LL, 100.75, 101, 2, 1},
};

// expectation corrected (kTrail_rows, 32 of 32 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   8585548509716714081ull -> 18024613014852726460ull
//   15473206481197801587ull -> 9833086322017794596ull
//   15916790782534920235ull -> 13932898073737259514ull
//   8505107563636241959ull -> 2431383563315972176ull
//   11165271446552335112ull -> 12687842165062574031ull
//   12822429560069742357ull -> 4980231387592577436ull
//   12964557168644427469ull -> 15960486502046552892ull
//   6583824150999662461ull -> 14852088680597802668ull
//   18110102307156631498ull -> 3300163549704231959ull
//   9893385568681652119ull -> 8833957126190935462ull
//   1023755713484717055ull -> 15572152602503413994ull
//   6402709505089761466ull -> 3878684154385141467ull
//   13122132210583281085ull -> 11390399893759060944ull
//   9523872496427661053ull -> 6663931660776036188ull
//   13553805667868534096ull -> 7943878597639294441ull
//   10866935271401923705ull -> 10527016018822989824ull
//   9616431850695140774ull -> 2438970678958820551ull
//   1433202945191076924ull -> 17299221755938893521ull
//   3650330492323816595ull -> 683754621688445586ull
//   9955028401963312566ull -> 15169428559726345055ull
//   7897194309223506885ull -> 4462520083872211988ull
//   6933188344695484411ull -> 4207496679375259778ull
//   6999968485656540596ull -> 12448221150014304093ull
//   2169376371561532223ull -> 4349660522993316974ull
//   15175663854668776200ull -> 1445567189232511149ull
//   4548155156087273091ull -> 12804379658110478862ull
//   17293184101531659580ull -> 6971917120656682009ull
//   6499157034190239763ull -> 1902642007883687518ull
//   6070282887470516250ull -> 4973932784755646067ull
//   15581087360767492421ull -> 1992576013327968808ull
//   12299302622417283557ull -> 8467178688432171320ull
//   2322910976412840875ull -> 11930736200467274982ull
// expectation corrected (v19-E, kTrail_rows, 32 of 32 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   18024613014852726460ull -> 10502385223359640218ull
//   9833086322017794596ull -> 11059645349770920252ull
//   13932898073737259514ull -> 12090607753233108862ull
//   2431383563315972176ull -> 4332510127470754579ull
//   12687842165062574031ull -> 2269674153403661467ull
//   4980231387592577436ull -> 1708485212322368238ull
//   15960486502046552892ull -> 15066207823399735690ull
//   14852088680597802668ull -> 17463983471023957816ull
//   3300163549704231959ull -> 10987770612284218667ull
//   8833957126190935462ull -> 8613262403787993426ull
//   15572152602503413994ull -> 10910001697921811365ull
//   3878684154385141467ull -> 13468567961207850309ull
//   11390399893759060944ull -> 17114810053420357407ull
//   6663931660776036188ull -> 13615766252800931030ull
//   7943878597639294441ull -> 11483795889704580492ull
//   10527016018822989824ull -> 3924269618188270202ull
//   2438970678958820551ull -> 4243281343999459289ull
//   17299221755938893521ull -> 9461891796031610162ull
//   683754621688445586ull -> 6122795060570411598ull
//   15169428559726345055ull -> 18091606639727212491ull
//   4462520083872211988ull -> 1843709319281047669ull
//   4207496679375259778ull -> 14954610474048933113ull
//   12448221150014304093ull -> 14845114560708251965ull
//   4349660522993316974ull -> 1504103451232493045ull
//   1445567189232511149ull -> 562770761915769535ull
//   12804379658110478862ull -> 848928317146655751ull
//   6971917120656682009ull -> 6260033633149799734ull
//   1902642007883687518ull -> 6655917714355362997ull
//   4973932784755646067ull -> 28864162607152614ull
//   1992576013327968808ull -> 15862752879538973131ull
//   8467178688432171320ull -> 9351225767820645871ull
//   11930736200467274982ull -> 11018457759974752048ull
// expectation corrected (V19-FIX, kTrail_rows, 32 of 32 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   10502385223359640218ull -> 11371589716169913082ull
//   11059645349770920252ull -> 15827480248058921916ull
//   12090607753233108862ull -> 9179483745920742590ull
//   4332510127470754579ull -> 6778316597629456467ull
//   2269674153403661467ull -> 5172328729500611739ull
//   1708485212322368238ull -> 538732652451172654ull
//   15066207823399735690ull -> 6878958150024727018ull
//   17463983471023957816ull -> 8673040670875734808ull
//   10987770612284218667ull -> 1224102971077480171ull
//   8613262403787993426ull -> 4557268190807619538ull
//   10910001697921811365ull -> 9136468491734931429ull
//   13468567961207850309ull -> 6184955078515723301ull
//   17114810053420357407ull -> 12362884735948053375ull
//   13615766252800931030ull -> 15431697961609933942ull
//   11483795889704580492ull -> 1411625524442821644ull
//   3924269618188270202ull -> 12832334111417447482ull
//   4243281343999459289ull -> 5598316292658291609ull
//   9461891796031610162ull -> 1094550630882239090ull
//   6122795060570411598ull -> 9032605003773573902ull
//   18091606639727212491ull -> 655750707938363819ull
//   1843709319281047669ull -> 11787846682663979157ull
//   14954610474048933113ull -> 4271553443345200153ull
//   14845114560708251965ull -> 13505617900634614461ull
//   1504103451232493045ull -> 14516777496449283573ull
//   562770761915769535ull -> 8741802295233492767ull
//   848928317146655751ull -> 7191771264792402823ull
//   6260033633149799734ull -> 407930561309081398ull
//   6655917714355362997ull -> 10139811205313313301ull
//   28864162607152614ull -> 9692372728761498854ull
//   15862752879538973131ull -> 17629340520421506187ull
//   9351225767820645871ull -> 12781001967037913295ull
//   11018457759974752048ull -> 4953750954703410960ull
// expectation corrected (INT26 v19 hash re-pin, kTrail_rows, 32 of 32 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   11371589716169913082ull -> 5794085833535179433ull [H-THIN]
//   15827480248058921916ull -> 2370845579388248975ull [H-THIN]
//   9179483745920742590ull -> 14433248242147310045ull [H-THIN]
//   6778316597629456467ull -> 6746208057179709300ull [H-THIN]
//   5172328729500611739ull -> 5141474228715449514ull [H-THIN]
//   538732652451172654ull -> 11607255782218151389ull [H-THIN]
//   6878958150024727018ull -> 15859825369433213587ull [H-THIN]
//   8673040670875734808ull -> 14000636333325119541ull [H-THIN]
//   1224102971077480171ull -> 8235513201704559620ull [H-THIN]
//   4557268190807619538ull -> 896476467432039189ull [H-THIN]
//   9136468491734931429ull -> 8318862700865263670ull [H-THIN]
//   6184955078515723301ull -> 7174198796690634212ull [H-THIN]
//   12362884735948053375ull -> 14047279371931353558ull [H-THIN]
//   15431697961609933942ull -> 10303898847732478779ull [H-THIN]
//   1411625524442821644ull -> 10919087129373839423ull [H-THIN]
//   12832334111417447482ull -> 2392471918185386717ull [H-THIN]
//   5598316292658291609ull -> 13975657224456405630ull [H-THIN]
//   1094550630882239090ull -> 5371073164686480833ull [H-THIN]
//   9032605003773573902ull -> 5788193476615953481ull [H-THIN]
//   655750707938363819ull -> 742735857153964210ull [H-THIN]
//   11787846682663979157ull -> 9511387276849347064ull [H-THIN]
//   4271553443345200153ull -> 8387023980895655564ull [H-THIN]
//   13505617900634614461ull -> 12129447825700318114ull [H-THIN]
//   14516777496449283573ull -> 6819473365388645714ull [H-THIN]
//   8741802295233492767ull -> 177891932096105070ull [H-THIN]
//   7191771264792402823ull -> 12347931419050467504ull [H-THIN]
//   407930561309081398ull -> 11096564370558447981ull [H-THIN]
//   10139811205313313301ull -> 5083341745670325276ull [H-THIN]
//   9692372728761498854ull -> 15979402051208842361ull [H-THIN]
//   17629340520421506187ull -> 10031744998823385352ull [H-THIN]
//   12781001967037913295ull -> 2405952843925529234ull [H-THIN]
//   4953750954703410960ull -> 15933138127006431029ull [H-THIN]
constexpr std::uint64_t kTrail_rows[] = {
    5794085833535179433ull,
    2370845579388248975ull,
    14433248242147310045ull,
    6746208057179709300ull,
    5141474228715449514ull,
    11607255782218151389ull,
    15859825369433213587ull,
    14000636333325119541ull,
    8235513201704559620ull,
    896476467432039189ull,
    8318862700865263670ull,
    7174198796690634212ull,
    14047279371931353558ull,
    10303898847732478779ull,
    10919087129373839423ull,
    2392471918185386717ull,
    13975657224456405630ull,
    5371073164686480833ull,
    5788193476615953481ull,
    742735857153964210ull,
    9511387276849347064ull,
    8387023980895655564ull,
    12129447825700318114ull,
    6819473365388645714ull,
    177891932096105070ull,
    12347931419050467504ull,
    11096564370558447981ull,
    5083341745670325276ull,
    15979402051208842361ull,
    10031744998823385352ull,
    2405952843925529234ull,
    15933138127006431029ull,
};
// expectation corrected (kTrail_reads, 2 of 2 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move:
//   3939475029491512578ull -> 12411711249896042591ull
//   7874578706403188786ull -> 791379823776991059ull
// expectation corrected (v19-E, kTrail_reads, 2 of 2 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   12411711249896042591ull -> 14558537749344697025ull
//   791379823776991059ull -> 12264623362903320190ull
// expectation corrected (V19-FIX, kTrail_reads, 2 of 2 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   14558537749344697025ull -> 9397758857253294369ull
//   12264623362903320190ull -> 8570208265068282686ull
// expectation corrected (INT26 v19 hash re-pin, kTrail_reads, 2 of 2 values), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move:
//   9397758857253294369ull -> 15035498073180564574ull [H-THIN]
//   8570208265068282686ull -> 2064235671204825645ull [H-THIN]
constexpr std::uint64_t kTrail_reads[] = {
    15035498073180564574ull,
    2064235671204825645ull,
};
// expectation corrected: kTrail_final 2322910976412840875ull -> 11930736200467274982ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); the trades did not move.
// expectation corrected (v19-E): kTrail_final 11930736200467274982ull -> 11018457759974752048ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
// expectation corrected (V19-FIX): kTrail_final 11018457759974752048ull -> 4953750954703410960ull, because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; the trades did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree.
// expectation corrected (INT26 v19 hash re-pin): kTrail_final 4953750954703410960ull -> 15933138127006431029ull [H-THIN], because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the trades did not move.
constexpr std::uint64_t kTrail_final = 15933138127006431029ull;
constexpr Trade kTrail_trades[] = {
    {1736121660000LL, 1736121840000LL, 100.25, 100.75, 2, 0},
};
// F9_PINNED_DATA_END

struct Expected {
    Scenario scenario;
    const std::uint64_t* rows;
    std::size_t rows_len;
    const std::uint64_t* reads;
    std::size_t reads_len;
    std::uint64_t final_hash;
    const Trade* trades;
    std::size_t trades_len;
};

#define F9_EXPECTED(S) {Scenario::S, k##S##_rows, std::size(k##S##_rows), k##S##_reads, \
    std::size(k##S##_reads), k##S##_final, k##S##_trades, std::size(k##S##_trades)}
constexpr Expected kExpected[] = {
    F9_EXPECTED(Submit), F9_EXPECTED(Replace), F9_EXPECTED(Cancel),
    F9_EXPECTED(Oca), F9_EXPECTED(Reissue), F9_EXPECTED(Trail),
};
#undef F9_EXPECTED

// The first observation that moved, so a failing fold names the point.
void same_values(const char* scenario, const char* what, const std::vector<std::uint64_t>& got,
                 const std::uint64_t* want, std::size_t want_len) {
    CHECK(got.size() == want_len);
    for (std::size_t i = 0; i < got.size() && i < want_len; ++i) {
        if (got[i] != want[i]) {
            std::fprintf(stderr, "%s %s[%zu]: observed %llu, pinned %llu\n", scenario, what, i,
                         static_cast<unsigned long long>(got[i]),
                         static_cast<unsigned long long>(want[i]));
            CHECK(got[i] == want[i]);
            return;
        }
    }
}

void same_trades(const char* scenario, const std::vector<Trade>& got, const Expected& want) {
    CHECK(got.size() == want.trades_len);
    for (std::size_t i = 0; i < got.size() && i < want.trades_len; ++i) {
        const Trade& a = got[i];
        const Trade& b = want.trades[i];
        const bool same = a.entry_time == b.entry_time && a.exit_time == b.exit_time
            && a.entry_price == b.entry_price && a.exit_price == b.exit_price
            && a.qty == b.qty && a.open_at_end == b.open_at_end;
        if (!same) std::fprintf(stderr, "%s trade[%zu] moved\n", scenario, i);
        CHECK(same);
    }
}

// Every observation point of one run against the pins: each recorded row,
// each read after a command, the final scalar, and the trades that say the
// scenario did what it describes.
void same_run(const Observed& got, const Expected& want, bool recording) {
    const char* scenario = name_of(want.scenario);
    if (recording) {
        same_values(scenario, "row", got.rows, want.rows, want.rows_len);
    } else {
        CHECK(got.rows.empty());
    }
    same_values(scenario, "read", got.reads, want.reads, want.reads_len);
    CHECK(got.final_hash == want.final_hash);
    same_trades(scenario, got.trades, want);
}
#endif

} // namespace

int main() {
#ifdef PINEFORGE_F9_HARVEST
    for (const auto scenario : kScenarios) emit(scenario, observe(scenario));
    return 0;
#else
    for (const auto& want : kExpected) {
        // A fresh host, recording.
        same_run(observe(want.scenario), want, true);
        // The same host run again: a reset answers the fresh values.
        WitnessHost reused(want.scenario, true);
        same_run(collect(reused, want.scenario), want, true);
        same_run(collect(reused, want.scenario), want, true);
        // Recording off: no rows, and the same state at every read.
        same_run(observe(want.scenario, false), want, false);
    }
    // The pins are non-trivial: every row of a scenario differs from the one
    // before, and the last row is the final scalar.
    for (const auto& want : kExpected) {
        for (std::size_t i = 1; i < want.rows_len; ++i) CHECK(want.rows[i] != want.rows[i - 1]);
        CHECK(want.rows_len != 0 && want.rows[want.rows_len - 1] == want.final_hash);
    }
    if (failures == 0)
        std::printf("test_adapter_recording_hash_witness: %d checks, 0 failures\n", checks);
    return failures == 0 ? 0 : 1;
#endif
}
