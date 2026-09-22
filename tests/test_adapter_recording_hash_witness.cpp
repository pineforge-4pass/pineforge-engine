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
constexpr std::uint64_t kSubmit_rows[] = {
    16709085573648202138ull,
    2367456619966436912ull,
    4494419410966626025ull,
    17380701833127534467ull,
    451619535107290686ull,
    2187293916418065859ull,
    4012538314749493373ull,
    10866247629710005993ull,
    17310929364016147338ull,
    12956826091814529166ull,
    9622783738306862249ull,
    8622803854841958074ull,
    4970368351605491565ull,
    8890797197972884845ull,
    5428705957761616462ull,
    8977647938089996694ull,
    13807565145524110216ull,
    2685926045958892414ull,
    681270338635768226ull,
    1234383668290826547ull,
    4756780818784761844ull,
    4691618259015575702ull,
    16054237718346851917ull,
    16534350264022081281ull,
    14658458716123961306ull,
    5627465258600142271ull,
    331903815701686263ull,
    4813090721847088203ull,
    12071930564723693486ull,
    7270151308451508039ull,
    2993593795212311439ull,
    4550614867809308423ull,
};
constexpr std::uint64_t kSubmit_reads[] = {
    10242542257977841923ull,
    16540927499285076903ull,
    6945018333701386048ull,
    7356558553176868704ull,
    11860308250432984141ull,
    5117593015133294249ull,
};
constexpr std::uint64_t kSubmit_final = 4550614867809308423ull;
constexpr Trade kSubmit_trades[] = {
    {1736121720000LL, 1736122260000LL, 101.5, 102, 2, 0},
    {1736122080000LL, 1736122620000LL, 100, 101, 2, 0},
    {1736122860000LL, 1736123220000LL, 102, 101.5, 2, 0},
};

constexpr std::uint64_t kReplace_rows[] = {
    16709085573648202138ull,
    15983598378715930306ull,
    4269588758895930903ull,
    11036824734939475591ull,
    12257656879758622505ull,
    14411164098109175801ull,
    9450757305780161736ull,
    7551970176279275465ull,
    14453287925757798752ull,
    12976323852396269092ull,
    5805001876393475955ull,
    10844959506524389683ull,
    13291722376204817347ull,
    11465826076836594596ull,
    11893948688539042135ull,
    8910467690674179911ull,
    12771933610374359049ull,
    11122028775837490723ull,
    7037834018034259618ull,
    17539147401022057209ull,
    5656548508765946794ull,
    16066430581990033300ull,
    13615537378955925529ull,
    9849716321944625332ull,
    9702250351256038096ull,
    4449732415226067191ull,
    9601235874939509895ull,
    13771220425752823706ull,
    13034380409707450981ull,
    10371866353517103554ull,
    11781937174085148984ull,
    17912935963855979970ull,
};
constexpr std::uint64_t kReplace_reads[] = {
    8242046592519750921ull,
    14188906364679548382ull,
    6575148672512319035ull,
    8722165113924581213ull,
    10537654301970811988ull,
    16831556078795966924ull,
    3306904984544048232ull,
    3367765439354791003ull,
};
constexpr std::uint64_t kReplace_final = 17912935963855979970ull;
constexpr Trade kReplace_trades[] = {
    {1736122080000LL, 1736122620000LL, 100.25, 101.25, 2, 0},
};

constexpr std::uint64_t kCancel_rows[] = {
    16709085573648202138ull,
    17453591279558544262ull,
    17575092659149777791ull,
    3211348821023947102ull,
    13458899738611866827ull,
    17722996774723384595ull,
    6428562136194339281ull,
    13084510485948270853ull,
    13760507618389236834ull,
    10358531465360738070ull,
    8303000664099948218ull,
    7402715195858175342ull,
    3035965499586260595ull,
    11719160029751101403ull,
    16206633647750508095ull,
    12855988691322821457ull,
    11120916648321494458ull,
    1893025614008146711ull,
    9547728811180246134ull,
    10584262505997145969ull,
    6103305640183140576ull,
    6345880683956586592ull,
    2078476119103271494ull,
    16311671080874283707ull,
    3020759032456607957ull,
    1513315884295622487ull,
    2311351656297275347ull,
    17082747341248638856ull,
    12868112467043389558ull,
    1507001438527628604ull,
    2334120587823506820ull,
    547718020446064204ull,
};
constexpr std::uint64_t kCancel_reads[] = {
    11036775732707302117ull,
    12173319204132133167ull,
    15089384555745740641ull,
    12657050168316361590ull,
    5508522320031760046ull,
    4392280703809030577ull,
    5672602513915569466ull,
    1584111208947804940ull,
    17604116988579061482ull,
    2571643557053314723ull,
    12263557600672438644ull,
    17566757678734830290ull,
    14239558782549945156ull,
    210949755977273831ull,
    12332984203645592185ull,
    2007155456688924841ull,
    3006055157799811198ull,
    8285142442694716882ull,
    14587662939274775057ull,
};
constexpr std::uint64_t kCancel_final = 547718020446064204ull;
constexpr Trade kCancel_trades[] = {
    {1736122980000LL, 1736123340000LL, 101, 102, 2, 0},
};

constexpr std::uint64_t kOca_rows[] = {
    16709085573648202138ull,
    15515119955049707023ull,
    2592424843452700379ull,
    16683291785383231136ull,
    17199212447046980747ull,
    2771767984845509636ull,
    9309510232087592598ull,
    15960215247508421371ull,
    7759688160476602112ull,
    17187218502565685484ull,
    6559742754627284760ull,
    18118180980177972209ull,
    13215094269827373432ull,
    13923831569574677590ull,
    7563273161250924677ull,
    5322278216926092479ull,
    16869350303802269268ull,
    16954816832214052155ull,
    3126570409416861252ull,
    2996293251300168498ull,
    9588882328803106067ull,
    9262140034190745255ull,
    16130298844759027972ull,
    15712758552780957086ull,
    12134094289949509338ull,
    14560200042856682656ull,
    17519121608160305268ull,
    16099910215857864057ull,
    7391386743554248102ull,
    18021125781548550855ull,
    16440759220253120341ull,
    13344165461917220241ull,
};
constexpr std::uint64_t kOca_reads[] = {
    14283998503469032055ull,
    1440851436719442360ull,
    13628616919967222108ull,
    3250156598192879549ull,
    5694049929555431913ull,
    8532241677982258469ull,
    10905738441214044975ull,
    5471479283459854020ull,
    13881602945166894983ull,
    922624683464432553ull,
    8476970287391158316ull,
    11307620409506918497ull,
};
constexpr std::uint64_t kOca_final = 13344165461917220241ull;
constexpr Trade kOca_trades[] = {
    {1736121960000LL, 1736122020000LL, 100.75, 100.75, 2, 0},
    {1736122500000LL, 1736122980000LL, 100.5, 101, 2, 0},
    {1736122680000LL, 1736122980000LL, 101, 101, 2, 0},
    {1736123400000LL, 1736123460000LL, 100.75, 100.75, 2, 0},
};

constexpr std::uint64_t kReissue_rows[] = {
    8585548509716714081ull,
    14325204492038295877ull,
    11333416975755677149ull,
    16543411225283864648ull,
    10938205834961117430ull,
    11387688170653017850ull,
    15953473043350382429ull,
    6572487799333293791ull,
    6528665896004934324ull,
    15865689254201632410ull,
    13278955049226971361ull,
    6953659889899964851ull,
    17379697670695539997ull,
    17872590505934490011ull,
    5749269872046101394ull,
    18391395619855151097ull,
    15052170923686004435ull,
    8530654778004631357ull,
    5149815064334227318ull,
    14245712945540619235ull,
    10733703939886245336ull,
    6273466604925818965ull,
    4079488818521470261ull,
    2146385853437671914ull,
    15888322699235513727ull,
    11387314208000124801ull,
    13309321548283340533ull,
    11569906020034529205ull,
    7391169599327399216ull,
    17943993331344782879ull,
    8161091785660856871ull,
    10597617675334596827ull,
};
constexpr std::uint64_t kReissue_reads[] = {
    3939475029491512578ull,
    7356730424947321612ull,
    4328265905674352487ull,
    15527784777326893165ull,
    9073698856330894556ull,
    2843133185829080314ull,
    18285520585784536762ull,
    3605867739876178206ull,
    11617385860051029808ull,
    6120070253966533302ull,
    12884089841196503522ull,
    1757057082581109267ull,
    15144596411273886687ull,
    16491034907638174255ull,
    17587854081778798790ull,
    7020653761431304951ull,
    6011692623971064780ull,
    16619768330427881043ull,
    15988479060284894684ull,
    16104785410922037962ull,
    9473156753073087859ull,
    12580998165444248416ull,
};
constexpr std::uint64_t kReissue_final = 10597617675334596827ull;
constexpr Trade kReissue_trades[] = {
    {1736121660000LL, 1736123040000LL, 100.75, 99.75, 2, 0},
    {1736123100000LL, 1736123460000LL, 100.75, 101, 2, 1},
};

constexpr std::uint64_t kTrail_rows[] = {
    8585548509716714081ull,
    15473206481197801587ull,
    15916790782534920235ull,
    8505107563636241959ull,
    11165271446552335112ull,
    12822429560069742357ull,
    12964557168644427469ull,
    6583824150999662461ull,
    18110102307156631498ull,
    9893385568681652119ull,
    1023755713484717055ull,
    6402709505089761466ull,
    13122132210583281085ull,
    9523872496427661053ull,
    13553805667868534096ull,
    10866935271401923705ull,
    9616431850695140774ull,
    1433202945191076924ull,
    3650330492323816595ull,
    9955028401963312566ull,
    7897194309223506885ull,
    6933188344695484411ull,
    6999968485656540596ull,
    2169376371561532223ull,
    15175663854668776200ull,
    4548155156087273091ull,
    17293184101531659580ull,
    6499157034190239763ull,
    6070282887470516250ull,
    15581087360767492421ull,
    12299302622417283557ull,
    2322910976412840875ull,
};
constexpr std::uint64_t kTrail_reads[] = {
    3939475029491512578ull,
    7874578706403188786ull,
};
constexpr std::uint64_t kTrail_final = 2322910976412840875ull;
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
