// R5 lane PERF-P4: the Pine adapter reads its receipts in place, and every
// value the reads feed is the one the materialised reads fed.
//
// The adapter reads the kernel's command events above its receipt cursor at
// three places: observe_terminal_receipts (three times a bar -- the bar open,
// the bar's calculation and the source publication), the exit precommit's
// same-bar declined-reversal test, and the margin call's bracket revival. Each
// used to materialise native_events(receipt_cursor_): an owning vector of every
// row above the cursor, driver points and account observations included. They
// now visit the command events in place (NativeExecutionConsumer::
// visit_commands_after) and advance the cursor row by row, then to the high
// water the materialised rows reached; observe_terminal_receipts also takes
// its no-new-event early return with the bar magnifier on, which it used not
// to. receipt_cursor_ and terminal_receipt_cursor_ are both folded into the
// broker-state hash (pine_state_hash.cpp), so the witness is data:
//
//   * receipt-heavy scenarios, each with the magnifier off and on -- a
//     declined in-position reversal that kills its bracket while the bracket's
//     stop is touched on the same bar and is re-issued later; the margin call
//     that revives such a bracket and the one whose revived stop cascades;
//     brackets whose filled leg cancels its siblings from inside the read
//     (the read appends while it is visited) and entries cancelled with their
//     pending brackets; cancel and reduce groups; a stop-limit activation --
//   * at every source bar, both cursors; at every hash the host folds (every
//     recorded report point and the final read), both cursors again; every
//     recorded broker-state hash row; the final scalar and every trade --
//
// observed on engine main fc7aad62 (the tree before this lane) and pinned
// here. Portability follows test_adapter_recording_hash_witness.cpp: the
// projection folds one fixed execution hash instead of the consumer's
// continuation (which folds tzdata content), and every price sits on its tick.
//
// And one property the materialised read could not have: a receipt read over
// a span that holds driver points and account rows but no command -- what two
// of a quiet bar's three reads see -- allocates nothing. This half FAILS on
// fc7aad62 (each such read built a vector), which is this row's fail-before.
//
// Provenance of the pinned data: this TU, compiled unchanged against the
// fc7aad62 library with -DPINEFORGE_P4_HARVEST, which prints the observed
// values as the initializers below instead of checking them (the allocation
// half is skipped there). Rebuild them the same way; never edit one by hand.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

// Counts the heap allocations of the commandless read: every replaceable
// form, one allocator.
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
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

// The adapter's two receipt cursors are private; read them the way
// test_l8c_adapter_ordering.cpp reads the adapter's tables.
template <class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
struct ReceiptCursorTag {
    using type = std::uint64_t source::PineExecutionAdapter::*;
    friend type access(ReceiptCursorTag);
};
template struct PrivateAccess<ReceiptCursorTag, &source::PineExecutionAdapter::receipt_cursor_>;
struct TerminalCursorTag {
    using type = std::uint64_t source::PineExecutionAdapter::*;
    friend type access(TerminalCursorTag);
};
template struct PrivateAccess<TerminalCursorTag,
                              &source::PineExecutionAdapter::terminal_receipt_cursor_>;

constexpr std::uint64_t kProbeExecutionHash = 0x5eed1234abcd00f4ull;
constexpr std::int64_t T = 1736121600000LL;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

enum class Scenario { Declined, Revive, Cascade, Brackets, Groups, StopLimit, Quiet };

const char* name_of(Scenario scenario) {
    switch (scenario) {
    case Scenario::Declined: return "Declined";
    case Scenario::Revive: return "Revive";
    case Scenario::Cascade: return "Cascade";
    case Scenario::Brackets: return "Brackets";
    case Scenario::Groups: return "Groups";
    case Scenario::StopLimit: return "StopLimit";
    case Scenario::Quiet: return "Quiet";
    }
    return "?";
}

Bar mk(int index, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, T + static_cast<std::int64_t>(index) * 60000};
}

// A triangular wave in quarter steps, the recording witness's tape.
std::vector<Bar> wave(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const int phase = i % 8;
        const int triangle = phase < 4 ? phase : 8 - phase;
        const double p = 100.0 + 0.5 * triangle + 0.25 * (i % 3);
        bars.push_back(mk(i, p, p + 0.5, p - 0.5, p + 0.25));
    }
    return bars;
}

std::vector<Bar> feed(Scenario scenario) {
    switch (scenario) {
    case Scenario::Declined:
        // LONG 100 @100 (bar 1), bar 1 closes 110 and the short reversal
        // queued there declines at bar 2's +1 gap; bar 2 touches the 90 stop
        // (the killed bracket must not fill), bar 5 re-issues it at 95 and
        // bar 6 crosses it.
        return {mk(0, 100, 100, 100, 100), mk(1, 100, 112, 99, 110),
                mk(2, 111, 112, 89, 111), mk(3, 111, 112, 88, 111),
                mk(4, 110, 110, 110, 110), mk(5, 110, 110, 110, 110),
                mk(6, 96, 97, 89, 95), mk(7, 95, 95, 95, 95),
                mk(8, 95, 96, 94, 95), mk(9, 95, 95, 95, 95)};
    case Scenario::Revive:
    case Scenario::Cascade:
        // A 5x short opens 100 @100 and bar 3's spike margin-calls a partial
        // that revives its dormant bracket. Cascade: the long reversal
        // declined at bar 2's +1 gap, and the revived stop 150 is marketable
        // at the call's own price. Revive: the reversal declines at the spike
        // bar's own open, so the call's revival reads that same-bar decline
        // first; the revived stop 180 fills on bar 4.
        return {mk(0, 100, 100, 100, 100), mk(1, 100, 101, 90, 90),
                mk(2, 91, 91, 91, 91), mk(3, 165, 170, 160, 168),
                scenario == Scenario::Revive ? mk(4, 175, 185, 170, 180)
                                             : mk(4, 168, 168, 168, 168),
                mk(5, 170, 171, 169, 170), mk(6, 170, 170, 170, 170)};
    case Scenario::Quiet:
        return wave(16);
    default:
        return wave(40);
    }
}

class WitnessHost final : public source::PineStrategyHost {
public:
    explicit WitnessHost(Scenario scenario) : scenario_(scenario) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        source::PineStrategyConfig config;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.0;
        config.initial_capital = 10000.0;
        switch (scenario) {
        case Scenario::Declined:
        case Scenario::Revive:
        case Scenario::Cascade:
            set_syminfo_mintick(0.01);
            config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
            config.default_qty_value = 100.0;
            config.pyramiding = 1;
            if (scenario != Scenario::Declined) config.margin_short = 20.0;
            set_margin_call_enabled(scenario != Scenario::Declined);
            break;
        default:
            set_syminfo_mintick(0.25);
            config.default_qty_type = static_cast<int>(QtyType::FIXED);
            config.default_qty_value = 2.0;
            config.pyramiding = 2;
            config.commission_value = 1.0;
            break;
        }
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
    }

    std::uint64_t broker_state_hash_projection() const override {
        return broker_state_hash_from_execution_hash(kProbeExecutionHash);
    }

    // Every hash this host folds: the recorded report points and the reads.
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        folded.push_back(receipt_cursor());
        folded.push_back(terminal_cursor());
    }

    std::uint64_t receipt_cursor() const { return adapter_.*access(ReceiptCursorTag{}); }
    std::uint64_t terminal_cursor() const { return adapter_.*access(TerminalCursorTag{}); }
    void rewind_receipt_cursor(std::uint64_t value) {
        adapter_.*access(ReceiptCursorTag{}) = value;
    }
    void observe_receipts() { adapter_.observe_terminal_receipts(); }

    mutable std::vector<std::uint64_t> folded;
    std::vector<std::uint64_t> at_bar;

    void on_source_bar(const Bar& bar) override {
        at_bar.push_back(receipt_cursor());
        at_bar.push_back(terminal_cursor());
        const int i = pine_bar_index();
        switch (scenario_) {
        case Scenario::Declined: declined(i); break;
        case Scenario::Revive: revive(i, 180.0, 2); break;
        case Scenario::Cascade: revive(i, 150.0, 1); break;
        case Scenario::Brackets: brackets(i, bar); break;
        case Scenario::Groups: groups(i, bar); break;
        case Scenario::StopLimit: stop_limit(i, bar); break;
        case Scenario::Quiet: quiet(i); break;
        }
    }

private:
    void declined(int i) {
        if (i == 0) strategy_entry("L", true);
        if (i == 1) {
            strategy_exit("X", "L", kNa, 90.0, kNa, kNa, kNa, 100.0, "");
            strategy_entry("S", false);
        }
        if (i == 5) strategy_exit("X", "L", kNa, 95.0, kNa, kNa, kNa, 100.0, "");
    }

    void revive(int i, double stop, int reversal_bar) {
        if (i == 0) {
            strategy_entry("S", false);
            strategy_exit("X", "S", kNa, stop, kNa, kNa, kNa, 100.0, "");
        }
        if (i == reversal_bar) strategy_entry("L", true);
    }

    // Two-leg brackets per cycle: a leg's fill cancels its sibling from inside
    // the receipt read. Every third cycle the entry is a far limit that is
    // cancelled while its bracket still waits for it.
    void brackets(int i, const Bar& bar) {
        const int phase = i % 6;
        const bool far = (i / 6) % 3 == 2;
        if (phase == 0) {
            if (far) strategy_entry("B", true, bar.close - 5.0);
            else strategy_entry("B", true);
            strategy_exit("tp", "B", bar.close + 0.75, bar.close - 1.5);
        }
        if (phase == 3 && far) strategy_cancel("B");
        if (phase == 5) strategy_close_all();
    }

    // Entry pairs in a cancel group (the first fill cancels its sibling) and
    // in a reduce group, and raw order pairs in the same kind of group: the
    // reduce pair's 1-unit fill reduces its 3-unit sibling.
    void groups(int i, const Bar& bar) {
        const int phase = i % 8;
        const bool reduce = (i / 8) % 2 == 1;
        const char* group = reduce ? "r" : "g";
        const int type = reduce ? 2 : 1;
        if (phase == 1) {
            strategy_entry("A", true, bar.close - 0.25, kNa, kNa, {}, group, type);
            strategy_entry("B", true, bar.close - 0.75, kNa, kNa, {}, group, type);
        }
        if (phase == 3) {
            strategy_order("o1", false, 1, bar.close + 0.5, kNa, "og", type);
            strategy_order("o2", false, 3, bar.close + 1.0, kNa, "og", type);
        }
        if (phase == 6) strategy_close_all();
    }

    // Stop-limit entries that activate and fill, or activate and wait.
    void stop_limit(int i, const Bar& bar) {
        const int phase = i % 5;
        if (phase == 0) strategy_entry("SL", true, bar.close + 0.25, bar.close + 0.5);
        if (phase == 2) strategy_order("SO", false, 1, bar.close - 0.75, bar.close - 0.25);
        if (phase == 4) {
            strategy_cancel_all();
            strategy_close_all();
        }
    }

    void quiet(int i) {
        if (i == 1) strategy_entry("Q", true);
        if (i == 3) strategy_close("Q");
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
    std::vector<std::uint64_t> at_bar;
    std::vector<std::uint64_t> folded;
    std::uint64_t final_hash = 0;
    std::vector<Trade> trades;
};

void run(WitnessHost& host, Scenario scenario, bool magnifier) {
    const auto bars = feed(scenario);
    if (magnifier) {
        host.run(bars.data(), static_cast<int>(bars.size()), "", "", true, 4,
                 MagnifierDistribution::ENDPOINTS);
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    CHECK(host.last_error().empty());
}

Observed observe(Scenario scenario, bool magnifier) {
    WitnessHost host(scenario);
    run(host, scenario, magnifier);
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
    out.final_hash = host.broker_state_hash();
    out.at_bar = host.at_bar;
    out.folded = host.folded;
    return out;
}

#ifdef PINEFORGE_P4_HARVEST
constexpr Scenario kScenarios[] = {Scenario::Declined, Scenario::Revive, Scenario::Cascade,
                                   Scenario::Brackets, Scenario::Groups, Scenario::StopLimit,
                                   Scenario::Quiet};

void emit_list(const char* symbol, const char* suffix, const std::vector<std::uint64_t>& values) {
    std::printf("constexpr std::uint64_t k%s_%s[] = {", symbol, suffix);
    for (std::size_t i = 0; i < values.size(); ++i)
        std::printf("%s%lluull,", i % 4 == 0 ? "\n    " : " ",
                    static_cast<unsigned long long>(values[i]));
    std::printf("\n};\n");
}

void emit(Scenario scenario, bool magnifier, const Observed& got) {
    char symbol[32];
    std::snprintf(symbol, sizeof symbol, "%s%s", name_of(scenario), magnifier ? "Mag" : "");
    emit_list(symbol, "rows", got.rows);
    emit_list(symbol, "at_bar", got.at_bar);
    emit_list(symbol, "folded", got.folded);
    std::printf("constexpr std::uint64_t k%s_final = %lluull;\n", symbol,
                static_cast<unsigned long long>(got.final_hash));
    std::printf("constexpr Trade k%s_trades[] = {\n", symbol);
    for (const auto& t : got.trades)
        std::printf("    {%lldLL, %lldLL, %.17g, %.17g, %.17g, %d},\n",
                    static_cast<long long>(t.entry_time), static_cast<long long>(t.exit_time),
                    t.entry_price, t.exit_price, t.qty, t.open_at_end);
    if (got.trades.empty()) std::printf("    {0, 0, 0, 0, 0, -1},\n");
    std::printf("};\n\n");
}
#else
// ── Pinned data (see the provenance note at the top of this file) ───────
// P4_PINNED_DATA_BEGIN
// expectation corrected (kDeclined_rows, 10 of 10 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   4285364442943983342ull -> 12856094836591424249ull
//   5064739043285013209ull -> 15504820197192426484ull
//   14226248201720924480ull -> 12871041340945864317ull
//   11756561249042112686ull -> 6809266845148365019ull
//   7502546032116966765ull -> 900047953638327288ull
//   9343248170671305998ull -> 14709301641892358505ull
//   16170761826422571350ull -> 574399613681696145ull
//   17156268346509373136ull -> 12574080240533717359ull
//   7683160624530869717ull -> 6097535005695266030ull
//   1667123404342696863ull -> 1945621519056434088ull
// expectation corrected (v19-E, kDeclined_rows, 10 of 10 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   12856094836591424249ull -> 10471252865781374815ull
//   15504820197192426484ull -> 7671906297724115341ull
//   12871041340945864317ull -> 2234871632290885580ull
//   6809266845148365019ull -> 11660583350044382565ull
//   900047953638327288ull -> 10795645000653949847ull
//   14709301641892358505ull -> 3876877678934974053ull
//   574399613681696145ull -> 8953460019459984272ull
//   12574080240533717359ull -> 901832621209212828ull
//   6097535005695266030ull -> 13528996515355458594ull
//   1945621519056434088ull -> 9684931586165891051ull
constexpr std::uint64_t kDeclined_rows[] = {
    10471252865781374815ull, 7671906297724115341ull, 2234871632290885580ull, 11660583350044382565ull,
    10795645000653949847ull, 3876877678934974053ull, 8953460019459984272ull, 901832621209212828ull,
    13528996515355458594ull, 9684931586165891051ull,
};
constexpr std::uint64_t kDeclined_at_bar[] = {
    4ull, 0ull, 12ull, 9ull,
    25ull, 24ull, 30ull, 24ull,
    35ull, 24ull, 40ull, 24ull,
    51ull, 49ull, 56ull, 49ull,
    61ull, 49ull, 66ull, 49ull,
};
constexpr std::uint64_t kDeclined_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    12ull, 9ull, 12ull, 9ull,
    25ull, 24ull, 25ull, 24ull,
    30ull, 24ull, 30ull, 24ull,
    35ull, 24ull, 35ull, 24ull,
    40ull, 24ull, 40ull, 24ull,
    51ull, 49ull, 51ull, 49ull,
    56ull, 49ull, 56ull, 49ull,
    61ull, 49ull, 61ull, 49ull,
    66ull, 49ull, 66ull, 49ull,
    66ull, 49ull,
};
// expectation corrected: kDeclined_final 1667123404342696863ull -> 1945621519056434088ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kDeclined_final 1945621519056434088ull -> 9684931586165891051ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kDeclined_final = 9684931586165891051ull;
constexpr Trade kDeclined_trades[] = {
    {1736121660000LL, 1736121960000LL, 100, 95, 100, 0},
};

// expectation corrected (kDeclinedMag_rows, 10 of 10 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   15827241547792344980ull -> 10227556164608664435ull
//   1131814333350244112ull -> 11630941325891769145ull
//   16157453329770597182ull -> 326800047495894143ull
//   13919762176745114404ull -> 3352632544817927005ull
//   9610884682524537723ull -> 9294351867547610146ull
//   16385497926780059888ull -> 11698155525352128255ull
//   11525863595680394547ull -> 4240970670313875044ull
//   1510992411965426849ull -> 12199814834755962046ull
//   14098169462848375672ull -> 14521223441703630547ull
//   16277588925252464314ull -> 6442860896266831741ull
// expectation corrected (v19-E, kDeclinedMag_rows, 10 of 10 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   10227556164608664435ull -> 2386459447381651821ull
//   11630941325891769145ull -> 6021168500418484266ull
//   326800047495894143ull -> 16446546055123594696ull
//   3352632544817927005ull -> 9282458332898651101ull
//   9294351867547610146ull -> 15456921776568509967ull
//   11698155525352128255ull -> 9589349023649693661ull
//   4240970670313875044ull -> 17301692639624276039ull
//   12199814834755962046ull -> 13176393922301188696ull
//   14521223441703630547ull -> 5693363764903822630ull
//   6442860896266831741ull -> 13182614947071830947ull
constexpr std::uint64_t kDeclinedMag_rows[] = {
    2386459447381651821ull, 6021168500418484266ull, 16446546055123594696ull, 9282458332898651101ull,
    15456921776568509967ull, 9589349023649693661ull, 17301692639624276039ull, 13176393922301188696ull,
    5693363764903822630ull, 13182614947071830947ull,
};
constexpr std::uint64_t kDeclinedMag_at_bar[] = {
    4ull, 0ull, 12ull, 0ull,
    25ull, 0ull, 30ull, 0ull,
    35ull, 0ull, 40ull, 0ull,
    51ull, 0ull, 56ull, 0ull,
    61ull, 0ull, 66ull, 0ull,
};
constexpr std::uint64_t kDeclinedMag_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    12ull, 0ull, 12ull, 0ull,
    25ull, 0ull, 25ull, 0ull,
    30ull, 0ull, 30ull, 0ull,
    35ull, 0ull, 35ull, 0ull,
    40ull, 0ull, 40ull, 0ull,
    51ull, 0ull, 51ull, 0ull,
    56ull, 0ull, 56ull, 0ull,
    61ull, 0ull, 61ull, 0ull,
    66ull, 0ull, 66ull, 0ull,
    66ull, 0ull,
};
// expectation corrected: kDeclinedMag_final 16277588925252464314ull -> 6442860896266831741ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kDeclinedMag_final 6442860896266831741ull -> 13182614947071830947ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kDeclinedMag_final = 13182614947071830947ull;
constexpr Trade kDeclinedMag_trades[] = {
    {1736121660000LL, 1736121960000LL, 100, 95, 100, 0},
};

// expectation corrected (kRevive_rows, 7 of 7 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   6350627700968043432ull -> 1794454665367904989ull
//   7893630477953678062ull -> 14476946673840322155ull
//   6286632809044419740ull -> 2878533217182850259ull
//   11489615726470058275ull -> 9087455859320809103ull
//   5071096704365483852ull -> 9246256503314484488ull
//   10124416207050257042ull -> 7573510687443965598ull
//   3480095526769689953ull -> 18153826259251315829ull
// expectation corrected (v19-E, kRevive_rows, 7 of 7 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   1794454665367904989ull -> 11381905052979082886ull
//   14476946673840322155ull -> 7711150994119040374ull
//   2878533217182850259ull -> 12033590382836458311ull
//   9087455859320809103ull -> 11344228619804537871ull
//   9246256503314484488ull -> 8695591448261818412ull
//   7573510687443965598ull -> 15937329709211664256ull
//   18153826259251315829ull -> 10634995016395674704ull
constexpr std::uint64_t kRevive_rows[] = {
    11381905052979082886ull, 7711150994119040374ull, 12033590382836458311ull, 11344228619804537871ull,
    8695591448261818412ull, 15937329709211664256ull, 10634995016395674704ull,
};
constexpr std::uint64_t kRevive_at_bar[] = {
    4ull, 0ull, 15ull, 10ull,
    20ull, 10ull, 33ull, 31ull,
    42ull, 40ull, 47ull, 40ull,
    52ull, 40ull,
};
constexpr std::uint64_t kRevive_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    15ull, 10ull, 15ull, 10ull,
    20ull, 10ull, 20ull, 10ull,
    33ull, 31ull, 33ull, 31ull,
    42ull, 40ull, 42ull, 40ull,
    47ull, 40ull, 47ull, 40ull,
    52ull, 40ull, 52ull, 40ull,
    52ull, 40ull,
};
// expectation corrected: kRevive_final 3480095526769689953ull -> 18153826259251315829ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kRevive_final 18153826259251315829ull -> 10634995016395674704ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kRevive_final = 10634995016395674704ull;
constexpr Trade kRevive_trades[] = {
    {1736121660000LL, 1736121780000LL, 100, 170, 47.058823529411768, 0},
    {1736121660000LL, 1736121840000LL, 100, 180, 52.941176470588232, 0},
};

// expectation corrected (kReviveMag_rows, 7 of 7 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   1579493378216782912ull -> 2770150853214632885ull
//   7511772062963861370ull -> 16577007729574383531ull
//   15379415014274107418ull -> 16612140979422814293ull
//   16105510747798700943ull -> 14269512506830746276ull
//   11290395518254980142ull -> 9108172091352313389ull
//   1443155722669931577ull -> 13205705026122939030ull
//   11170248427254383026ull -> 11245820422146263909ull
// expectation corrected (v19-E, kReviveMag_rows, 7 of 7 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   2770150853214632885ull -> 14608739080977528633ull
//   16577007729574383531ull -> 3414911638394215429ull
//   16612140979422814293ull -> 13152262788765868075ull
//   14269512506830746276ull -> 9127833906595294915ull
//   9108172091352313389ull -> 9719574618747977040ull
//   13205705026122939030ull -> 16705937982676897010ull
//   11245820422146263909ull -> 403749858405626294ull
constexpr std::uint64_t kReviveMag_rows[] = {
    14608739080977528633ull, 3414911638394215429ull, 13152262788765868075ull, 9127833906595294915ull,
    9719574618747977040ull, 16705937982676897010ull, 403749858405626294ull,
};
constexpr std::uint64_t kReviveMag_at_bar[] = {
    4ull, 0ull, 15ull, 0ull,
    20ull, 0ull, 28ull, 0ull,
    40ull, 0ull, 45ull, 0ull,
    50ull, 0ull,
};
constexpr std::uint64_t kReviveMag_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    15ull, 0ull, 15ull, 0ull,
    20ull, 0ull, 20ull, 0ull,
    28ull, 0ull, 28ull, 0ull,
    40ull, 0ull, 40ull, 0ull,
    45ull, 0ull, 45ull, 0ull,
    50ull, 0ull, 50ull, 0ull,
    50ull, 0ull,
};
// expectation corrected: kReviveMag_final 11170248427254383026ull -> 11245820422146263909ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kReviveMag_final 11245820422146263909ull -> 403749858405626294ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kReviveMag_final = 403749858405626294ull;
constexpr Trade kReviveMag_trades[] = {
    {1736121660000LL, 1736121840000LL, 100, 175, 100, 0},
};

// expectation corrected (kCascade_rows, 7 of 7 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   11803825725282039008ull -> 14201675796734872109ull
//   11121191306941517363ull -> 14883226082450713864ull
//   3178279434697480731ull -> 9210413751886863980ull
//   7425460857334141232ull -> 5088729369761968278ull
//   8350079040744008319ull -> 16254425278702601285ull
//   9723365308680356972ull -> 12056339472046727078ull
//   17244746764465919736ull -> 5566037634063544638ull
// expectation corrected (v19-E, kCascade_rows, 7 of 7 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   14201675796734872109ull -> 7093386270889089222ull
//   14883226082450713864ull -> 18110309630310117569ull
//   9210413751886863980ull -> 6621058774966980276ull
//   5088729369761968278ull -> 8896173852282142540ull
//   16254425278702601285ull -> 10109285908224112591ull
//   12056339472046727078ull -> 10060812652362367792ull
//   5566037634063544638ull -> 12854015805238324476ull
constexpr std::uint64_t kCascade_rows[] = {
    7093386270889089222ull, 18110309630310117569ull, 6621058774966980276ull, 8896173852282142540ull,
    10109285908224112591ull, 10060812652362367792ull, 12854015805238324476ull,
};
constexpr std::uint64_t kCascade_at_bar[] = {
    4ull, 0ull, 15ull, 10ull,
    23ull, 20ull, 43ull, 40ull,
    48ull, 40ull, 53ull, 40ull,
    58ull, 40ull,
};
constexpr std::uint64_t kCascade_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    15ull, 10ull, 15ull, 10ull,
    23ull, 20ull, 23ull, 20ull,
    43ull, 40ull, 43ull, 40ull,
    48ull, 40ull, 48ull, 40ull,
    53ull, 40ull, 53ull, 40ull,
    58ull, 40ull, 58ull, 40ull,
    58ull, 40ull,
};
// expectation corrected: kCascade_final 17244746764465919736ull -> 5566037634063544638ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kCascade_final 5566037634063544638ull -> 12854015805238324476ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kCascade_final = 12854015805238324476ull;
constexpr Trade kCascade_trades[] = {
    {1736121660000LL, 1736121780000LL, 100, 170, 47.058823529411768, 0},
    {1736121660000LL, 1736121780000LL, 100, 170, 52.941176470588232, 0},
};

// expectation corrected (kCascadeMag_rows, 7 of 7 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   15795341388107798312ull -> 16943719637917289333ull
//   4633146533710855253ull -> 3033759523742299742ull
//   10768521223239694579ull -> 11349978307766473204ull
//   15472347828572140999ull -> 16332863668003752352ull
//   16884913969937812814ull -> 6423023268752600282ull
//   12305957632002004150ull -> 7685185556116772882ull
//   12442414818770313178ull -> 8438336155355340534ull
// expectation corrected (v19-E, kCascadeMag_rows, 7 of 7 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   16943719637917289333ull -> 4473825047915283433ull
//   3033759523742299742ull -> 227172169403486164ull
//   11349978307766473204ull -> 17659296469268981537ull
//   16332863668003752352ull -> 2132167081514435162ull
//   6423023268752600282ull -> 2820795946605591416ull
//   7685185556116772882ull -> 8320281576492738244ull
//   8438336155355340534ull -> 8969517760469763972ull
constexpr std::uint64_t kCascadeMag_rows[] = {
    4473825047915283433ull, 227172169403486164ull, 17659296469268981537ull, 2132167081514435162ull,
    2820795946605591416ull, 8320281576492738244ull, 8969517760469763972ull,
};
constexpr std::uint64_t kCascadeMag_at_bar[] = {
    4ull, 0ull, 15ull, 0ull,
    23ull, 0ull, 31ull, 0ull,
    48ull, 0ull, 53ull, 0ull,
    58ull, 0ull,
};
constexpr std::uint64_t kCascadeMag_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    15ull, 0ull, 15ull, 0ull,
    23ull, 0ull, 23ull, 0ull,
    31ull, 0ull, 31ull, 0ull,
    48ull, 0ull, 48ull, 0ull,
    53ull, 0ull, 53ull, 0ull,
    58ull, 0ull, 58ull, 0ull,
    58ull, 0ull,
};
// expectation corrected: kCascadeMag_final 12442414818770313178ull -> 8438336155355340534ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kCascadeMag_final 8438336155355340534ull -> 8969517760469763972ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kCascadeMag_final = 8969517760469763972ull;
constexpr Trade kCascadeMag_trades[] = {
    {1736121660000LL, 1736121840000LL, 100, 168, 19.047619047619047, 0},
    {1736121660000LL, 1736121840000LL, 100, 168, 80.952380952380949, 0},
};

// expectation corrected (kBrackets_rows, 40 of 40 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   14866027288923674590ull -> 15964723208263105807ull
//   16649853781238903429ull -> 18167170959488607721ull
//   13190095744217365753ull -> 16169761435293925477ull
//   10502269067036593532ull -> 12011658759925385000ull
//   18381143185443891561ull -> 10161593434535165421ull
//   1735915102031019572ull -> 17061908903949940064ull
//   10395586145912073704ull -> 17680583138796774976ull
//   2679523764946147230ull -> 12233693849697488992ull
//   18384433671636462098ull -> 11541888490153081288ull
//   18211235479487566787ull -> 6853463793584716081ull
//   18277897802945420404ull -> 13496246752569413642ull
//   17395674248469413919ull -> 2561324668713836543ull
//   13220479791033372227ull -> 395666586849707255ull
//   15569212298721506404ull -> 7620328467879252344ull
//   8779802303491361775ull -> 13393655897258609067ull
//   5372534098409924662ull -> 13072820731949733170ull
//   15552749709575985492ull -> 4115376090645326072ull
//   13902135963118532762ull -> 7558769655308128342ull
//   1437248562486350733ull -> 11099653503289572593ull
//   10518682552051823326ull -> 12349995993275513478ull
//   14625840823528391271ull -> 9853439019586956095ull
//   6916250278448210655ull -> 3124830029817120215ull
//   16347660827743144968ull -> 1789614075613897488ull
//   5193341365299488091ull -> 10656288234390810131ull
//   12591925847622054552ull -> 5815036086040214556ull
//   5455453127300802186ull -> 7611236857217259922ull
//   15337733870115412176ull -> 9955249106055794584ull
//   13393536876314999769ull -> 5050601228851247521ull
//   14506760582926131442ull -> 3158046515094860010ull
//   6398254635611204925ull -> 2178640860738556549ull
//   16786661275526125283ull -> 3640007144478631287ull
//   11680808028246450544ull -> 4700362308750533044ull
//   10881585127984591333ull -> 15601257983251237561ull
//   10699952175155992651ull -> 7859975024587756231ull
//   3957098159049250088ull -> 12688105520529228132ull
//   12537604784772314763ull -> 8429390325579926175ull
//   14032309735782374896ull -> 16099194608118513320ull
//   13456059580862079650ull -> 16414089404670494488ull
//   7207516763549747550ull -> 2984278679623361172ull
//   12275462523833622145ull -> 15408496303561531855ull
// expectation corrected (v19-E, kBrackets_rows, 40 of 40 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   15964723208263105807ull -> 4931640785442886345ull
//   18167170959488607721ull -> 17478909815634119236ull
//   16169761435293925477ull -> 15352487419733065212ull
//   12011658759925385000ull -> 12654730253382767814ull
//   10161593434535165421ull -> 189181168054283670ull
//   17061908903949940064ull -> 12777030162683706874ull
//   17680583138796774976ull -> 4366298018774171687ull
//   12233693849697488992ull -> 16302107181139381722ull
//   11541888490153081288ull -> 2490861395787358522ull
//   6853463793584716081ull -> 6163669939419568773ull
//   13496246752569413642ull -> 14839279797350066889ull
//   2561324668713836543ull -> 11327888575643522147ull
//   395666586849707255ull -> 13772309817367178166ull
//   7620328467879252344ull -> 11073935078124632822ull
//   13393655897258609067ull -> 6509820274401670617ull
//   13072820731949733170ull -> 12808134870016028653ull
//   4115376090645326072ull -> 12922818780252687683ull
//   7558769655308128342ull -> 5540434211183952058ull
//   11099653503289572593ull -> 17372884883543544148ull
//   12349995993275513478ull -> 4458440122004238600ull
//   9853439019586956095ull -> 4906307060214704524ull
//   3124830029817120215ull -> 539100643123332448ull
//   1789614075613897488ull -> 17897281496465220788ull
//   10656288234390810131ull -> 6899239783095585150ull
//   5815036086040214556ull -> 120086616464741494ull
//   7611236857217259922ull -> 2311613447022880607ull
//   9955249106055794584ull -> 16955448480744754862ull
//   5050601228851247521ull -> 1159311057332895795ull
//   3158046515094860010ull -> 3742824900740526962ull
//   2178640860738556549ull -> 17966647797995324035ull
//   3640007144478631287ull -> 5514998760284978445ull
//   4700362308750533044ull -> 6213880116988350109ull
//   15601257983251237561ull -> 7311426441934803793ull
//   7859975024587756231ull -> 4767957879114898923ull
//   12688105520529228132ull -> 17766099843733535304ull
//   8429390325579926175ull -> 7744375361093502469ull
//   16099194608118513320ull -> 1502110319718444822ull
//   16414089404670494488ull -> 9088484067275656470ull
//   2984278679623361172ull -> 11937924564662907819ull
//   15408496303561531855ull -> 7215366721055711217ull
constexpr std::uint64_t kBrackets_rows[] = {
    4931640785442886345ull, 17478909815634119236ull, 15352487419733065212ull, 12654730253382767814ull,
    189181168054283670ull, 12777030162683706874ull, 4366298018774171687ull, 16302107181139381722ull,
    2490861395787358522ull, 6163669939419568773ull, 14839279797350066889ull, 11327888575643522147ull,
    13772309817367178166ull, 11073935078124632822ull, 6509820274401670617ull, 12808134870016028653ull,
    12922818780252687683ull, 5540434211183952058ull, 17372884883543544148ull, 4458440122004238600ull,
    4906307060214704524ull, 539100643123332448ull, 17897281496465220788ull, 6899239783095585150ull,
    120086616464741494ull, 2311613447022880607ull, 16955448480744754862ull, 1159311057332895795ull,
    3742824900740526962ull, 17966647797995324035ull, 5514998760284978445ull, 6213880116988350109ull,
    7311426441934803793ull, 4767957879114898923ull, 17766099843733535304ull, 7744375361093502469ull,
    1502110319718444822ull, 9088484067275656470ull, 11937924564662907819ull, 7215366721055711217ull,
};
constexpr std::uint64_t kBrackets_at_bar[] = {
    4ull, 0ull, 25ull, 20ull,
    29ull, 20ull, 34ull, 20ull,
    39ull, 20ull, 44ull, 20ull,
    49ull, 20ull, 63ull, 56ull,
    68ull, 56ull, 73ull, 56ull,
    78ull, 56ull, 90ull, 83ull,
    94ull, 83ull, 102ull, 83ull,
    107ull, 83ull, 112ull, 83ull,
    120ull, 117ull, 125ull, 117ull,
    130ull, 117ull, 151ull, 146ull,
    155ull, 146ull, 160ull, 146ull,
    165ull, 146ull, 170ull, 146ull,
    175ull, 146ull, 196ull, 191ull,
    200ull, 191ull, 205ull, 191ull,
    210ull, 191ull, 215ull, 191ull,
    220ull, 191ull, 228ull, 191ull,
    233ull, 191ull, 238ull, 191ull,
    246ull, 243ull, 251ull, 243ull,
    256ull, 243ull, 270ull, 263ull,
    275ull, 263ull, 288ull, 281ull,
};
constexpr std::uint64_t kBrackets_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    25ull, 20ull, 25ull, 20ull,
    29ull, 20ull, 29ull, 20ull,
    34ull, 20ull, 34ull, 20ull,
    39ull, 20ull, 39ull, 20ull,
    44ull, 20ull, 44ull, 20ull,
    49ull, 20ull, 49ull, 20ull,
    63ull, 56ull, 63ull, 56ull,
    68ull, 56ull, 68ull, 56ull,
    73ull, 56ull, 73ull, 56ull,
    78ull, 56ull, 78ull, 56ull,
    90ull, 83ull, 90ull, 83ull,
    94ull, 83ull, 94ull, 83ull,
    102ull, 83ull, 102ull, 83ull,
    107ull, 83ull, 107ull, 83ull,
    112ull, 83ull, 112ull, 83ull,
    120ull, 117ull, 120ull, 117ull,
    125ull, 117ull, 125ull, 117ull,
    130ull, 117ull, 130ull, 117ull,
    151ull, 146ull, 151ull, 146ull,
    155ull, 146ull, 155ull, 146ull,
    160ull, 146ull, 160ull, 146ull,
    165ull, 146ull, 165ull, 146ull,
    170ull, 146ull, 170ull, 146ull,
    175ull, 146ull, 175ull, 146ull,
    196ull, 191ull, 196ull, 191ull,
    200ull, 191ull, 200ull, 191ull,
    205ull, 191ull, 205ull, 191ull,
    210ull, 191ull, 210ull, 191ull,
    215ull, 191ull, 215ull, 191ull,
    220ull, 191ull, 220ull, 191ull,
    228ull, 191ull, 228ull, 191ull,
    233ull, 191ull, 233ull, 191ull,
    238ull, 191ull, 238ull, 191ull,
    246ull, 243ull, 246ull, 243ull,
    251ull, 243ull, 251ull, 243ull,
    256ull, 243ull, 256ull, 243ull,
    270ull, 263ull, 270ull, 263ull,
    275ull, 263ull, 275ull, 263ull,
    288ull, 281ull, 288ull, 281ull,
    288ull, 281ull,
};
// expectation corrected: kBrackets_final 12275462523833622145ull -> 15408496303561531855ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kBrackets_final 15408496303561531855ull -> 7215366721055711217ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kBrackets_final = 7215366721055711217ull;
constexpr Trade kBrackets_trades[] = {
    {1736121660000LL, 1736121660000LL, 100.75, 101, 2, 0},
    {1736122020000LL, 1736122260000LL, 100.75, 102, 2, 0},
    {1736122740000LL, 1736122740000LL, 101.75, 102, 2, 0},
    {1736123100000LL, 1736123100000LL, 100.75, 101, 2, 0},
    {1736123820000LL, 1736123940000LL, 101.75, 100.5, 2, 0},
};

// expectation corrected (kBracketsMag_rows, 40 of 40 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   14925296382428985648ull -> 2066977806072931245ull
//   7344905789884533150ull -> 16983144648714401210ull
//   16090565159374661598ull -> 15154732921304022786ull
//   11225360465726061199ull -> 7430506637177048339ull
//   4423772731596842302ull -> 6139822229162389754ull
//   1864118206373791711ull -> 16078416733277273651ull
//   16170709559648037869ull -> 12841894959897223357ull
//   481673215253245186ull -> 14191204585146456892ull
//   14209727281670483637ull -> 3622843178346799375ull
//   5231187342479144464ull -> 17301208792735827426ull
//   12956579884983977310ull -> 14046178675709204112ull
//   16877300195273226901ull -> 4765550430594314301ull
//   3518124402729136401ull -> 7483030404471798105ull
//   18194591487925347118ull -> 7997128395546901670ull
//   11275175479190670649ull -> 840332958357165777ull
//   1727531436455231448ull -> 740478509675061952ull
//   2812906101421470624ull -> 722644947950996648ull
//   14507886661839533642ull -> 14954960252344997250ull
//   13272932003274552705ull -> 9105803746374110633ull
//   1254610278521633748ull -> 7502034589005664338ull
//   5296589554740508017ull -> 4912899242642006383ull
//   5020844913341177189ull -> 6536977231217234571ull
//   6534636244690225506ull -> 9818191934865809756ull
//   18437577580270433421ull -> 1947166204241191135ull
//   13673021230426178520ull -> 9948292666280213466ull
//   10160723947616790626ull -> 12129204467206152165ull
//   600322673604262576ull -> 18397505290517640095ull
//   11221611113249085965ull -> 3484496395255014318ull
//   2328355405364402798ull -> 10158940191120117573ull
//   11300656303009114157ull -> 3599900080891250006ull
//   16771700425934511859ull -> 2523352643665786976ull
//   11126451407672334260ull -> 6090758879656369927ull
//   14182501097978637485ull -> 4535377656382922198ull
//   4654667524987087133ull -> 7330426164098921620ull
//   13979336787298677508ull -> 2739228614497042995ull
//   5593389497923022859ull -> 2584605907442310936ull
//   2748819722934504052ull -> 4968335839723864259ull
//   3185167942597426079ull -> 11230894092276043958ull
//   6037389201975695465ull -> 15687066908260860816ull
//   4480275903615486608ull -> 16366316158039625809ull
// expectation corrected (v19-E, kBracketsMag_rows, 40 of 40 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   2066977806072931245ull -> 357160593917001728ull
//   16983144648714401210ull -> 1367088814885629166ull
//   15154732921304022786ull -> 287323235851632294ull
//   7430506637177048339ull -> 16045756699822731600ull
//   6139822229162389754ull -> 13135689560889444372ull
//   16078416733277273651ull -> 11854950712426707972ull
//   12841894959897223357ull -> 7925187025432663817ull
//   14191204585146456892ull -> 6633703321685341395ull
//   3622843178346799375ull -> 4104072211950622040ull
//   17301208792735827426ull -> 7434875672168557443ull
//   14046178675709204112ull -> 5786698095493413500ull
//   4765550430594314301ull -> 16482929592882674954ull
//   7483030404471798105ull -> 5269590940989060304ull
//   7997128395546901670ull -> 8058999920878049372ull
//   840332958357165777ull -> 14585381469692231775ull
//   740478509675061952ull -> 1138810438475977261ull
//   722644947950996648ull -> 260823923338859754ull
//   14954960252344997250ull -> 688478973399870179ull
//   9105803746374110633ull -> 650200918600910455ull
//   7502034589005664338ull -> 12588212143642844613ull
//   4912899242642006383ull -> 4094759726207491032ull
//   6536977231217234571ull -> 13503618892702370984ull
//   9818191934865809756ull -> 17750941632463851516ull
//   1947166204241191135ull -> 15151194576654354422ull
//   9948292666280213466ull -> 6825948494367863103ull
//   12129204467206152165ull -> 17303881736078502733ull
//   18397505290517640095ull -> 18179306564848873449ull
//   3484496395255014318ull -> 771952960889069628ull
//   10158940191120117573ull -> 16658405033057303029ull
//   3599900080891250006ull -> 14621560559856470332ull
//   2523352643665786976ull -> 5398287334306475188ull
//   6090758879656369927ull -> 4872986020265265668ull
//   4535377656382922198ull -> 9997838899172977900ull
//   7330426164098921620ull -> 6944843506869705438ull
//   2739228614497042995ull -> 3332727370045095070ull
//   2584605907442310936ull -> 5092799111316890531ull
//   4968335839723864259ull -> 4451780574198029219ull
//   11230894092276043958ull -> 13669172117143703422ull
//   15687066908260860816ull -> 5803040722061771621ull
//   16366316158039625809ull -> 10498718013143678943ull
constexpr std::uint64_t kBracketsMag_rows[] = {
    357160593917001728ull, 1367088814885629166ull, 287323235851632294ull, 16045756699822731600ull,
    13135689560889444372ull, 11854950712426707972ull, 7925187025432663817ull, 6633703321685341395ull,
    4104072211950622040ull, 7434875672168557443ull, 5786698095493413500ull, 16482929592882674954ull,
    5269590940989060304ull, 8058999920878049372ull, 14585381469692231775ull, 1138810438475977261ull,
    260823923338859754ull, 688478973399870179ull, 650200918600910455ull, 12588212143642844613ull,
    4094759726207491032ull, 13503618892702370984ull, 17750941632463851516ull, 15151194576654354422ull,
    6825948494367863103ull, 17303881736078502733ull, 18179306564848873449ull, 771952960889069628ull,
    16658405033057303029ull, 14621560559856470332ull, 5398287334306475188ull, 4872986020265265668ull,
    9997838899172977900ull, 6944843506869705438ull, 3332727370045095070ull, 5092799111316890531ull,
    4451780574198029219ull, 13669172117143703422ull, 5803040722061771621ull, 10498718013143678943ull,
};
constexpr std::uint64_t kBracketsMag_at_bar[] = {
    4ull, 0ull, 25ull, 0ull,
    29ull, 0ull, 34ull, 0ull,
    39ull, 0ull, 44ull, 0ull,
    49ull, 0ull, 63ull, 0ull,
    68ull, 0ull, 73ull, 0ull,
    78ull, 0ull, 90ull, 0ull,
    94ull, 0ull, 102ull, 0ull,
    107ull, 0ull, 112ull, 0ull,
    120ull, 0ull, 125ull, 0ull,
    130ull, 0ull, 151ull, 0ull,
    155ull, 0ull, 160ull, 0ull,
    165ull, 0ull, 170ull, 0ull,
    175ull, 0ull, 196ull, 0ull,
    200ull, 0ull, 205ull, 0ull,
    210ull, 0ull, 215ull, 0ull,
    220ull, 0ull, 228ull, 0ull,
    233ull, 0ull, 238ull, 0ull,
    246ull, 0ull, 251ull, 0ull,
    256ull, 0ull, 270ull, 0ull,
    275ull, 0ull, 288ull, 0ull,
};
constexpr std::uint64_t kBracketsMag_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    25ull, 0ull, 25ull, 0ull,
    29ull, 0ull, 29ull, 0ull,
    34ull, 0ull, 34ull, 0ull,
    39ull, 0ull, 39ull, 0ull,
    44ull, 0ull, 44ull, 0ull,
    49ull, 0ull, 49ull, 0ull,
    63ull, 0ull, 63ull, 0ull,
    68ull, 0ull, 68ull, 0ull,
    73ull, 0ull, 73ull, 0ull,
    78ull, 0ull, 78ull, 0ull,
    90ull, 0ull, 90ull, 0ull,
    94ull, 0ull, 94ull, 0ull,
    102ull, 0ull, 102ull, 0ull,
    107ull, 0ull, 107ull, 0ull,
    112ull, 0ull, 112ull, 0ull,
    120ull, 0ull, 120ull, 0ull,
    125ull, 0ull, 125ull, 0ull,
    130ull, 0ull, 130ull, 0ull,
    151ull, 0ull, 151ull, 0ull,
    155ull, 0ull, 155ull, 0ull,
    160ull, 0ull, 160ull, 0ull,
    165ull, 0ull, 165ull, 0ull,
    170ull, 0ull, 170ull, 0ull,
    175ull, 0ull, 175ull, 0ull,
    196ull, 0ull, 196ull, 0ull,
    200ull, 0ull, 200ull, 0ull,
    205ull, 0ull, 205ull, 0ull,
    210ull, 0ull, 210ull, 0ull,
    215ull, 0ull, 215ull, 0ull,
    220ull, 0ull, 220ull, 0ull,
    228ull, 0ull, 228ull, 0ull,
    233ull, 0ull, 233ull, 0ull,
    238ull, 0ull, 238ull, 0ull,
    246ull, 0ull, 246ull, 0ull,
    251ull, 0ull, 251ull, 0ull,
    256ull, 0ull, 256ull, 0ull,
    270ull, 0ull, 270ull, 0ull,
    275ull, 0ull, 275ull, 0ull,
    288ull, 0ull, 288ull, 0ull,
    288ull, 0ull,
};
// expectation corrected: kBracketsMag_final 4480275903615486608ull -> 16366316158039625809ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kBracketsMag_final 16366316158039625809ull -> 10498718013143678943ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kBracketsMag_final = 10498718013143678943ull;
constexpr Trade kBracketsMag_trades[] = {
    {1736121660000LL, 1736121660000LL, 100.75, 101.25, 2, 0},
    {1736122020000LL, 1736122260000LL, 100.75, 102, 2, 0},
    {1736122740000LL, 1736122740000LL, 101.75, 102.25, 2, 0},
    {1736123100000LL, 1736123100000LL, 100.75, 101.25, 2, 0},
    {1736123820000LL, 1736123940000LL, 101.75, 100.5, 2, 0},
};

// expectation corrected (kGroups_rows, 40 of 40 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   7724584198093564595ull -> 16959315319659428706ull
//   14037535911552261334ull -> 9662782895242222699ull
//   14964184649618471106ull -> 17816916981472989463ull
//   15837640502650876441ull -> 16783326897651268624ull
//   5841811833025652711ull -> 8627445157846423902ull
//   6880597703884275978ull -> 13208584798542801635ull
//   4022005563316796046ull -> 2101662584429674147ull
//   11646411279043485842ull -> 8350221208381710507ull
//   2025125997917000074ull -> 11023914496373191959ull
//   14963035782174266194ull -> 8970719140330989017ull
//   13703469485999267191ull -> 8809128085283224552ull
//   11785310893319292714ull -> 15134200687472324629ull
//   5562700109292854673ull -> 12839064763128685398ull
//   13051181478567279819ull -> 2450199763159819404ull
//   15362637042632761092ull -> 14489847597850597811ull
//   11337152299125080166ull -> 12503781073680909675ull
//   14366095805963646679ull -> 15648754151211460078ull
//   13006999377430155062ull -> 16403540582549544943ull
//   18251237808884358198ull -> 17245322386798152001ull
//   3367226118791643879ull -> 17702050786000427762ull
//   10112133094528063767ull -> 4027212867442561517ull
//   5632786340700576450ull -> 2797898948342005516ull
//   9542058346449055539ull -> 5138107884381572717ull
//   5336767761041012898ull -> 16832838614521747926ull
//   5050327092814624473ull -> 1285807415224659549ull
//   9259344525213473815ull -> 6129991478908546495ull
//   14339458378944358911ull -> 5082936380358681415ull
//   11531371741958840313ull -> 4027714083808551061ull
//   6841871301419080046ull -> 287899484953659282ull
//   16333070905054532344ull -> 10206789455209287636ull
//   15431932348706501099ull -> 1589168646241316596ull
//   7926864697956809263ull -> 12931580635097389404ull
//   175881041745163829ull -> 13827449540800183354ull
//   1160678453166274165ull -> 17058419353919972154ull
//   624762104954538763ull -> 9767049155939025580ull
//   9950397847729605829ull -> 18150435143310974930ull
//   7375119601134904582ull -> 4966203015418151185ull
//   5972030537660888252ull -> 7461966957553468455ull
//   10485455422255788959ull -> 8230861333685497064ull
//   11093578271132015941ull -> 2748401044783380800ull
// expectation corrected (v19-E, kGroups_rows, 40 of 40 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   16959315319659428706ull -> 2251190330680026869ull
//   9662782895242222699ull -> 7887834686192888166ull
//   17816916981472989463ull -> 3855968558637463435ull
//   16783326897651268624ull -> 2546062310896875234ull
//   8627445157846423902ull -> 600437454323137621ull
//   13208584798542801635ull -> 16674487917741010732ull
//   2101662584429674147ull -> 4610683579624476637ull
//   8350221208381710507ull -> 15092384830490081514ull
//   11023914496373191959ull -> 14071508027291531173ull
//   8970719140330989017ull -> 6111309149023879865ull
//   8809128085283224552ull -> 18103735437698418633ull
//   15134200687472324629ull -> 3959243710668744815ull
//   12839064763128685398ull -> 3952210482411450856ull
//   2450199763159819404ull -> 3490143314173465755ull
//   14489847597850597811ull -> 14538602064706366786ull
//   12503781073680909675ull -> 5218037555664020499ull
//   15648754151211460078ull -> 225708733966286232ull
//   16403540582549544943ull -> 9782719631545392594ull
//   17245322386798152001ull -> 14940244590911598032ull
//   17702050786000427762ull -> 12346330238722784478ull
//   4027212867442561517ull -> 13467268729476158219ull
//   2797898948342005516ull -> 16944155163988596044ull
//   5138107884381572717ull -> 3768322604539017323ull
//   16832838614521747926ull -> 17328156936260981351ull
//   1285807415224659549ull -> 4766285883157907692ull
//   6129991478908546495ull -> 11123152710249472138ull
//   5082936380358681415ull -> 1712228033902812862ull
//   4027714083808551061ull -> 8137321895545044533ull
//   287899484953659282ull -> 4135452977285764992ull
//   10206789455209287636ull -> 1035023176731878356ull
//   1589168646241316596ull -> 10069202913180375095ull
//   12931580635097389404ull -> 12120236779844291703ull
//   13827449540800183354ull -> 3940468084958498734ull
//   17058419353919972154ull -> 16394284185178237666ull
//   9767049155939025580ull -> 1347962035407032299ull
//   18150435143310974930ull -> 11984308608154771468ull
//   4966203015418151185ull -> 14161529370115150918ull
//   7461966957553468455ull -> 7242134762483671893ull
//   8230861333685497064ull -> 1865370244366172271ull
//   2748401044783380800ull -> 10203291295434148760ull
constexpr std::uint64_t kGroups_rows[] = {
    2251190330680026869ull, 7887834686192888166ull, 3855968558637463435ull, 2546062310896875234ull,
    600437454323137621ull, 16674487917741010732ull, 4610683579624476637ull, 15092384830490081514ull,
    14071508027291531173ull, 6111309149023879865ull, 18103735437698418633ull, 3959243710668744815ull,
    3952210482411450856ull, 3490143314173465755ull, 14538602064706366786ull, 5218037555664020499ull,
    225708733966286232ull, 9782719631545392594ull, 14940244590911598032ull, 12346330238722784478ull,
    13467268729476158219ull, 16944155163988596044ull, 3768322604539017323ull, 17328156936260981351ull,
    4766285883157907692ull, 11123152710249472138ull, 1712228033902812862ull, 8137321895545044533ull,
    4135452977285764992ull, 1035023176731878356ull, 10069202913180375095ull, 12120236779844291703ull,
    3940468084958498734ull, 16394284185178237666ull, 1347962035407032299ull, 11984308608154771468ull,
    14161529370115150918ull, 7242134762483671893ull, 1865370244366172271ull, 10203291295434148760ull,
};
constexpr std::uint64_t kGroups_at_bar[] = {
    4ull, 0ull, 9ull, 0ull,
    16ull, 0ull, 21ull, 0ull,
    31ull, 28ull, 36ull, 28ull,
    42ull, 40ull, 53ull, 51ull,
    58ull, 51ull, 63ull, 51ull,
    69ull, 51ull, 74ull, 51ull,
    81ull, 51ull, 86ull, 51ull,
    91ull, 51ull, 102ull, 98ull,
    107ull, 98ull, 112ull, 98ull,
    122ull, 119ull, 127ull, 119ull,
    137ull, 134ull, 142ull, 134ull,
    147ull, 134ull, 156ull, 152ull,
    161ull, 152ull, 166ull, 152ull,
    173ull, 152ull, 178ull, 152ull,
    189ull, 188ull, 194ull, 188ull,
    202ull, 199ull, 213ull, 211ull,
    218ull, 211ull, 223ull, 211ull,
    230ull, 211ull, 235ull, 211ull,
    242ull, 211ull, 247ull, 211ull,
    252ull, 211ull, 260ull, 257ull,
};
constexpr std::uint64_t kGroups_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    9ull, 0ull, 9ull, 0ull,
    16ull, 0ull, 16ull, 0ull,
    21ull, 0ull, 21ull, 0ull,
    31ull, 28ull, 31ull, 28ull,
    36ull, 28ull, 36ull, 28ull,
    42ull, 40ull, 42ull, 40ull,
    53ull, 51ull, 53ull, 51ull,
    58ull, 51ull, 58ull, 51ull,
    63ull, 51ull, 63ull, 51ull,
    69ull, 51ull, 69ull, 51ull,
    74ull, 51ull, 74ull, 51ull,
    81ull, 51ull, 81ull, 51ull,
    86ull, 51ull, 86ull, 51ull,
    91ull, 51ull, 91ull, 51ull,
    102ull, 98ull, 102ull, 98ull,
    107ull, 98ull, 107ull, 98ull,
    112ull, 98ull, 112ull, 98ull,
    122ull, 119ull, 122ull, 119ull,
    127ull, 119ull, 127ull, 119ull,
    137ull, 134ull, 137ull, 134ull,
    142ull, 134ull, 142ull, 134ull,
    147ull, 134ull, 147ull, 134ull,
    156ull, 152ull, 156ull, 152ull,
    161ull, 152ull, 161ull, 152ull,
    166ull, 152ull, 166ull, 152ull,
    173ull, 152ull, 173ull, 152ull,
    178ull, 152ull, 178ull, 152ull,
    189ull, 188ull, 189ull, 188ull,
    194ull, 188ull, 194ull, 188ull,
    202ull, 199ull, 202ull, 199ull,
    213ull, 211ull, 213ull, 211ull,
    218ull, 211ull, 218ull, 211ull,
    223ull, 211ull, 223ull, 211ull,
    230ull, 211ull, 230ull, 211ull,
    235ull, 211ull, 235ull, 211ull,
    242ull, 211ull, 242ull, 211ull,
    247ull, 211ull, 247ull, 211ull,
    252ull, 211ull, 252ull, 211ull,
    260ull, 257ull, 260ull, 257ull,
    260ull, 257ull,
};
// expectation corrected: kGroups_final 11093578271132015941ull -> 2748401044783380800ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kGroups_final 2748401044783380800ull -> 10203291295434148760ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kGroups_final = 10203291295434148760ull;
constexpr Trade kGroups_trades[] = {
    {1736121840000LL, 1736122020000LL, 102.25, 100.75, 1, 0},
    {1736122020000LL, 1736122500000LL, 100.25, 100.5, 2, 0},
    {1736122500000LL, 1736122500000LL, 100.5, 100.5, 2, 0},
    {1736122680000LL, 1736122800000LL, 101, 102.5, 1, 0},
    {1736122680000LL, 1736122980000LL, 101, 101, 1, 0},
    {1736123280000LL, 1736123400000LL, 102.25, 100.75, 1, 0},
    {1736123280000LL, 1736123400000LL, 102.75, 100.75, 1, 0},
    {1736123280000LL, 1736123460000LL, 102.75, 100.75, 1, 0},
    {1736123940000LL, 1736123940000LL, 100.5, 100.75, 2, 1},
};

// expectation corrected (kGroupsMag_rows, 40 of 40 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   5733365443022361725ull -> 1611239520478642856ull
//   1770031795907811498ull -> 5866513899520841783ull
//   6470203284857417282ull -> 6508685523332948271ull
//   13680705116515132995ull -> 11711942189022294022ull
//   16280306007782621573ull -> 10251787494203051888ull
//   17886876073090196864ull -> 17222088111894449605ull
//   13570091299407770576ull -> 10002193254540806393ull
//   17049172465269546805ull -> 12128502854266259852ull
//   6168640773089004757ull -> 5215002702401697160ull
//   4304075059045850359ull -> 14664867756512403864ull
//   13725508340902668646ull -> 7778537362290504957ull
//   3951890106122286393ull -> 1929768523184405330ull
//   9085666311990288374ull -> 3074823437879012325ull
//   12954585096737187612ull -> 2742646780404396623ull
//   6795169276410185767ull -> 7180335848611606196ull
//   12139316376347823538ull -> 2571569628461962507ull
//   6087488785891629379ull -> 17195420035517546006ull
//   3648533416536772720ull -> 12525717851995742497ull
//   16869591150172377173ull -> 1063826586028025722ull
//   14514487283608884298ull -> 10808329718739825167ull
//   12777372886064072601ull -> 13912096044489095787ull
//   8639486342548277404ull -> 13042906520217320818ull
//   14296087827019584825ull -> 1442615894010468807ull
//   7513667181080739246ull -> 14799580633817596562ull
//   16005201315071742289ull -> 11935762791987006229ull
//   7683997310820400007ull -> 1721066826117845343ull
//   274527616443811739ull -> 5770688305792740627ull
//   13774001486483371441ull -> 2952829295520316285ull
//   2038523230773680902ull -> 6397349821788388202ull
//   4394512243199420690ull -> 12284527337305917294ull
//   6875720906349168873ull -> 14846602744029861853ull
//   3027886066460029894ull -> 10479854529203197188ull
//   12678959196294236002ull -> 5432128453790480032ull
//   11136522415607694552ull -> 12143796197516788982ull
//   2000687809818107336ull -> 6044096889576589326ull
//   4060242198356548732ull -> 9866588356308180182ull
//   14187333568294550129ull -> 1393789522186000127ull
//   12558284470601575633ull -> 7651563840620793587ull
//   13092014301223548584ull -> 10759678117498166630ull
//   8471333527175518852ull -> 17786987883651578348ull
// expectation corrected (v19-E, kGroupsMag_rows, 40 of 40 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   1611239520478642856ull -> 1771071767131182639ull
//   5866513899520841783ull -> 2319264234820563061ull
//   6508685523332948271ull -> 3968516947995724204ull
//   11711942189022294022ull -> 13748929559384796587ull
//   10251787494203051888ull -> 14337866189563969968ull
//   17222088111894449605ull -> 121300902977360074ull
//   10002193254540806393ull -> 13930328507840437447ull
//   12128502854266259852ull -> 11293074932416602936ull
//   5215002702401697160ull -> 17806644710829561148ull
//   14664867756512403864ull -> 11345590561057098901ull
//   7778537362290504957ull -> 16975877748262402349ull
//   1929768523184405330ull -> 10562941317592452443ull
//   3074823437879012325ull -> 12227261626285808416ull
//   2742646780404396623ull -> 3101225148965538227ull
//   7180335848611606196ull -> 3221854327620463942ull
//   2571569628461962507ull -> 17879106457247694620ull
//   17195420035517546006ull -> 12516768081916550737ull
//   12525717851995742497ull -> 1139783168782783090ull
//   1063826586028025722ull -> 12488380805259075729ull
//   10808329718739825167ull -> 17657402560985447898ull
//   13912096044489095787ull -> 329254244800689628ull
//   13042906520217320818ull -> 3662055418137315433ull
//   1442615894010468807ull -> 7047152031506478874ull
//   14799580633817596562ull -> 1983174958676155844ull
//   11935762791987006229ull -> 923225174573989794ull
//   1721066826117845343ull -> 18280876562855262839ull
//   5770688305792740627ull -> 13719457309818612247ull
//   2952829295520316285ull -> 1969493590092048305ull
//   6397349821788388202ull -> 4113075276561899954ull
//   12284527337305917294ull -> 13646979131324573792ull
//   14846602744029861853ull -> 15106529513333095294ull
//   10479854529203197188ull -> 17890990718808582694ull
//   5432128453790480032ull -> 6506735898811973185ull
//   12143796197516788982ull -> 9170129921256422232ull
//   6044096889576589326ull -> 7575139446008419839ull
//   9866588356308180182ull -> 11761619212592004855ull
//   1393789522186000127ull -> 7566433005087203435ull
//   7651563840620793587ull -> 6230002762718235250ull
//   10759678117498166630ull -> 14578437808706486766ull
//   17786987883651578348ull -> 12557658352255323163ull
constexpr std::uint64_t kGroupsMag_rows[] = {
    1771071767131182639ull, 2319264234820563061ull, 3968516947995724204ull, 13748929559384796587ull,
    14337866189563969968ull, 121300902977360074ull, 13930328507840437447ull, 11293074932416602936ull,
    17806644710829561148ull, 11345590561057098901ull, 16975877748262402349ull, 10562941317592452443ull,
    12227261626285808416ull, 3101225148965538227ull, 3221854327620463942ull, 17879106457247694620ull,
    12516768081916550737ull, 1139783168782783090ull, 12488380805259075729ull, 17657402560985447898ull,
    329254244800689628ull, 3662055418137315433ull, 7047152031506478874ull, 1983174958676155844ull,
    923225174573989794ull, 18280876562855262839ull, 13719457309818612247ull, 1969493590092048305ull,
    4113075276561899954ull, 13646979131324573792ull, 15106529513333095294ull, 17890990718808582694ull,
    6506735898811973185ull, 9170129921256422232ull, 7575139446008419839ull, 11761619212592004855ull,
    7566433005087203435ull, 6230002762718235250ull, 14578437808706486766ull, 12557658352255323163ull,
};
constexpr std::uint64_t kGroupsMag_at_bar[] = {
    4ull, 0ull, 9ull, 0ull,
    16ull, 0ull, 21ull, 0ull,
    31ull, 0ull, 36ull, 0ull,
    42ull, 0ull, 53ull, 0ull,
    58ull, 0ull, 63ull, 0ull,
    69ull, 0ull, 74ull, 0ull,
    81ull, 0ull, 86ull, 0ull,
    91ull, 0ull, 102ull, 0ull,
    107ull, 0ull, 112ull, 0ull,
    122ull, 0ull, 127ull, 0ull,
    137ull, 0ull, 142ull, 0ull,
    147ull, 0ull, 156ull, 0ull,
    161ull, 0ull, 166ull, 0ull,
    173ull, 0ull, 178ull, 0ull,
    188ull, 0ull, 193ull, 0ull,
    201ull, 0ull, 212ull, 0ull,
    217ull, 0ull, 222ull, 0ull,
    229ull, 0ull, 234ull, 0ull,
    241ull, 0ull, 246ull, 0ull,
    251ull, 0ull, 259ull, 0ull,
};
constexpr std::uint64_t kGroupsMag_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    9ull, 0ull, 9ull, 0ull,
    16ull, 0ull, 16ull, 0ull,
    21ull, 0ull, 21ull, 0ull,
    31ull, 0ull, 31ull, 0ull,
    36ull, 0ull, 36ull, 0ull,
    42ull, 0ull, 42ull, 0ull,
    53ull, 0ull, 53ull, 0ull,
    58ull, 0ull, 58ull, 0ull,
    63ull, 0ull, 63ull, 0ull,
    69ull, 0ull, 69ull, 0ull,
    74ull, 0ull, 74ull, 0ull,
    81ull, 0ull, 81ull, 0ull,
    86ull, 0ull, 86ull, 0ull,
    91ull, 0ull, 91ull, 0ull,
    102ull, 0ull, 102ull, 0ull,
    107ull, 0ull, 107ull, 0ull,
    112ull, 0ull, 112ull, 0ull,
    122ull, 0ull, 122ull, 0ull,
    127ull, 0ull, 127ull, 0ull,
    137ull, 0ull, 137ull, 0ull,
    142ull, 0ull, 142ull, 0ull,
    147ull, 0ull, 147ull, 0ull,
    156ull, 0ull, 156ull, 0ull,
    161ull, 0ull, 161ull, 0ull,
    166ull, 0ull, 166ull, 0ull,
    173ull, 0ull, 173ull, 0ull,
    178ull, 0ull, 178ull, 0ull,
    188ull, 0ull, 188ull, 0ull,
    193ull, 0ull, 193ull, 0ull,
    201ull, 0ull, 201ull, 0ull,
    212ull, 0ull, 212ull, 0ull,
    217ull, 0ull, 217ull, 0ull,
    222ull, 0ull, 222ull, 0ull,
    229ull, 0ull, 229ull, 0ull,
    234ull, 0ull, 234ull, 0ull,
    241ull, 0ull, 241ull, 0ull,
    246ull, 0ull, 246ull, 0ull,
    251ull, 0ull, 251ull, 0ull,
    259ull, 0ull, 259ull, 0ull,
    259ull, 0ull,
};
// expectation corrected: kGroupsMag_final 8471333527175518852ull -> 17786987883651578348ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kGroupsMag_final 17786987883651578348ull -> 12557658352255323163ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kGroupsMag_final = 12557658352255323163ull;
constexpr Trade kGroupsMag_trades[] = {
    {1736121840000LL, 1736122020000LL, 102.25, 100.75, 1, 0},
    {1736122020000LL, 1736122500000LL, 100.25, 100.5, 2, 0},
    {1736122500000LL, 1736122500000LL, 100.5, 100.5, 2, 0},
    {1736122680000LL, 1736122800000LL, 101, 102.5, 1, 0},
    {1736122680000LL, 1736122980000LL, 101, 101, 1, 0},
    {1736123280000LL, 1736123400000LL, 102.25, 100.5, 1, 0},
    {1736123280000LL, 1736123400000LL, 102.75, 100.5, 1, 0},
    {1736123280000LL, 1736123460000LL, 102.75, 100.75, 1, 0},
    {1736123940000LL, 1736123940000LL, 100.5, 100.75, 2, 1},
};

// expectation corrected (kStopLimit_rows, 40 of 40 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   2951874735862626799ull -> 8489558650205257992ull
//   11001154575678754787ull -> 1294089400587576112ull
//   10191383546300744753ull -> 10408648325823307924ull
//   14335067750512523113ull -> 13057567868469817223ull
//   14634254702333820496ull -> 5124930489296941566ull
//   13727563301137820252ull -> 8032318101430847993ull
//   7732167413016711558ull -> 8806267729229901059ull
//   23989540955952803ull -> 8462655079904816236ull
//   17868341505123005444ull -> 10485881222165035635ull
//   8901216489246200621ull -> 14592593682418187518ull
//   7235870513229523578ull -> 16841726040197290805ull
//   14347351966641657743ull -> 6555602609672438232ull
//   15514857036218605458ull -> 14434623748445992455ull
//   3292206687046947377ull -> 14051566628621942990ull
//   11664433890693965409ull -> 8890352622069103778ull
//   12016486425273185683ull -> 853635413734509765ull
//   7858597352620855107ull -> 7483303061794291537ull
//   7409416796933497235ull -> 7121018970733460249ull
//   15475869289132129140ull -> 17005532270068863705ull
//   6794288551174200691ull -> 3402121060917503150ull
//   5047559130542054898ull -> 5065629011111772531ull
//   4592577991936859864ull -> 18363107041150180305ull
//   4145923563897710011ull -> 2087128851181154404ull
//   14084652476072856286ull -> 553307028252726773ull
//   4367276615515885720ull -> 12687692202466792159ull
//   4103383789594518773ull -> 15079180650406769633ull
//   4078016538705775748ull -> 15020015480588732124ull
//   12900085140413569457ull -> 15587580212155046753ull
//   281955135076910959ull -> 14103358619619938607ull
//   1219123596768957497ull -> 2757695372969081788ull
//   2593395659259851147ull -> 1983292994267332780ull
//   13366497277076464801ull -> 12780173733914770702ull
//   14903972582846578408ull -> 6533977571769018103ull
//   4655963223934593536ull -> 4622107224101528739ull
//   14602679593300513726ull -> 8283480650920382073ull
//   7172060859019788156ull -> 218931548665103213ull
//   10076308138673754946ull -> 16310980435283688031ull
//   2251278248701969261ull -> 18011553307993037888ull
//   7350541109135979421ull -> 9349597393465852520ull
//   2398349164106856788ull -> 3665904849405917965ull
// expectation corrected (v19-E, kStopLimit_rows, 40 of 40 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   8489558650205257992ull -> 4015678377754031763ull
//   1294089400587576112ull -> 9277256445902250361ull
//   10408648325823307924ull -> 7787615283403152830ull
//   13057567868469817223ull -> 9402632213295178026ull
//   5124930489296941566ull -> 15777447031447382573ull
//   8032318101430847993ull -> 1608910765213702288ull
//   8806267729229901059ull -> 8731220795971939073ull
//   8462655079904816236ull -> 5600775303740731350ull
//   10485881222165035635ull -> 6788919839149980292ull
//   14592593682418187518ull -> 17608363530245642196ull
//   16841726040197290805ull -> 18227871926589828513ull
//   6555602609672438232ull -> 9190871252466246446ull
//   14434623748445992455ull -> 13585156238110074232ull
//   14051566628621942990ull -> 4377935914087343184ull
//   8890352622069103778ull -> 484123207012063444ull
//   853635413734509765ull -> 16208949663969524763ull
//   7483303061794291537ull -> 12701173708241788309ull
//   7121018970733460249ull -> 15919837620478786311ull
//   17005532270068863705ull -> 7210516455815976589ull
//   3402121060917503150ull -> 10435577839346943286ull
//   5065629011111772531ull -> 16020477384123899678ull
//   18363107041150180305ull -> 6460320043547129038ull
//   2087128851181154404ull -> 2173034416395231487ull
//   553307028252726773ull -> 1745821339674292435ull
//   12687692202466792159ull -> 2929233302710984265ull
//   15079180650406769633ull -> 5366701996650499488ull
//   15020015480588732124ull -> 9271513753148892055ull
//   15587580212155046753ull -> 4539748013823005514ull
//   14103358619619938607ull -> 15991039173201268618ull
//   2757695372969081788ull -> 13840553339973172792ull
//   1983292994267332780ull -> 848639617528874544ull
//   12780173733914770702ull -> 3081441532474388176ull
//   6533977571769018103ull -> 16913374911412348286ull
//   4622107224101528739ull -> 3098179540339637900ull
//   8283480650920382073ull -> 16397906308196786061ull
//   218931548665103213ull -> 16552233276333719913ull
//   16310980435283688031ull -> 17559531532379237941ull
//   18011553307993037888ull -> 1600250331183838308ull
//   9349597393465852520ull -> 11097052730880109561ull
//   3665904849405917965ull -> 13378853722070711842ull
constexpr std::uint64_t kStopLimit_rows[] = {
    4015678377754031763ull, 9277256445902250361ull, 7787615283403152830ull, 9402632213295178026ull,
    15777447031447382573ull, 1608910765213702288ull, 8731220795971939073ull, 5600775303740731350ull,
    6788919839149980292ull, 17608363530245642196ull, 18227871926589828513ull, 9190871252466246446ull,
    13585156238110074232ull, 4377935914087343184ull, 484123207012063444ull, 16208949663969524763ull,
    12701173708241788309ull, 15919837620478786311ull, 7210516455815976589ull, 10435577839346943286ull,
    16020477384123899678ull, 6460320043547129038ull, 2173034416395231487ull, 1745821339674292435ull,
    2929233302710984265ull, 5366701996650499488ull, 9271513753148892055ull, 4539748013823005514ull,
    15991039173201268618ull, 13840553339973172792ull, 848639617528874544ull, 3081441532474388176ull,
    16913374911412348286ull, 3098179540339637900ull, 16397906308196786061ull, 16552233276333719913ull,
    17559531532379237941ull, 1600250331183838308ull, 11097052730880109561ull, 13378853722070711842ull,
};
constexpr std::uint64_t kStopLimit_at_bar[] = {
    4ull, 0ull, 13ull, 11ull,
    18ull, 11ull, 26ull, 23ull,
    31ull, 23ull, 40ull, 36ull,
    46ull, 36ull, 51ull, 36ull,
    59ull, 56ull, 64ull, 56ull,
    74ull, 70ull, 83ull, 81ull,
    88ull, 81ull, 96ull, 93ull,
    101ull, 93ull, 110ull, 106ull,
    116ull, 106ull, 122ull, 106ull,
    132ull, 129ull, 137ull, 129ull,
    146ull, 142ull, 152ull, 142ull,
    157ull, 142ull, 165ull, 162ull,
    170ull, 162ull, 180ull, 176ull,
    189ull, 187ull, 194ull, 187ull,
    200ull, 187ull, 207ull, 205ull,
    216ull, 212ull, 222ull, 212ull,
    227ull, 212ull, 235ull, 232ull,
    242ull, 242ull, 251ull, 247ull,
    257ull, 247ull, 262ull, 247ull,
    270ull, 267ull, 275ull, 267ull,
};
constexpr std::uint64_t kStopLimit_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    13ull, 11ull, 13ull, 11ull,
    18ull, 11ull, 18ull, 11ull,
    26ull, 23ull, 26ull, 23ull,
    31ull, 23ull, 31ull, 23ull,
    40ull, 36ull, 40ull, 36ull,
    46ull, 36ull, 46ull, 36ull,
    51ull, 36ull, 51ull, 36ull,
    59ull, 56ull, 59ull, 56ull,
    64ull, 56ull, 64ull, 56ull,
    74ull, 70ull, 74ull, 70ull,
    83ull, 81ull, 83ull, 81ull,
    88ull, 81ull, 88ull, 81ull,
    96ull, 93ull, 96ull, 93ull,
    101ull, 93ull, 101ull, 93ull,
    110ull, 106ull, 110ull, 106ull,
    116ull, 106ull, 116ull, 106ull,
    122ull, 106ull, 122ull, 106ull,
    132ull, 129ull, 132ull, 129ull,
    137ull, 129ull, 137ull, 129ull,
    146ull, 142ull, 146ull, 142ull,
    152ull, 142ull, 152ull, 142ull,
    157ull, 142ull, 157ull, 142ull,
    165ull, 162ull, 165ull, 162ull,
    170ull, 162ull, 170ull, 162ull,
    180ull, 176ull, 180ull, 176ull,
    189ull, 187ull, 189ull, 187ull,
    194ull, 187ull, 194ull, 187ull,
    200ull, 187ull, 200ull, 187ull,
    207ull, 205ull, 207ull, 205ull,
    216ull, 212ull, 216ull, 212ull,
    222ull, 212ull, 222ull, 212ull,
    227ull, 212ull, 227ull, 212ull,
    235ull, 232ull, 235ull, 232ull,
    242ull, 242ull, 242ull, 242ull,
    251ull, 247ull, 251ull, 247ull,
    257ull, 247ull, 257ull, 247ull,
    262ull, 247ull, 262ull, 247ull,
    270ull, 267ull, 270ull, 267ull,
    275ull, 267ull, 275ull, 267ull,
    275ull, 267ull,
};
// expectation corrected: kStopLimit_final 2398349164106856788ull -> 3665904849405917965ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kStopLimit_final 3665904849405917965ull -> 13378853722070711842ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kStopLimit_final = 13378853722070711842ull;
constexpr Trade kStopLimit_trades[] = {
    {1736121660000LL, 1736121780000LL, 100.5, 101.5, 1, 0},
    {1736121660000LL, 1736121900000LL, 100.5, 102, 1, 0},
    {1736122080000LL, 1736122200000LL, 100.5, 101.25, 1, 0},
    {1736122260000LL, 1736122380000LL, 101.75, 101.75, 1, 0},
    {1736122260000LL, 1736122500000LL, 101.75, 100.5, 1, 0},
    {1736122680000LL, 1736122680000LL, 101, 101, 1, 0},
    {1736122680000LL, 1736122800000LL, 101, 102.5, 1, 0},
    {1736122980000LL, 1736123100000LL, 101, 100.75, 1, 0},
    {1736123160000LL, 1736123340000LL, 101.25, 101.5, 1, 0},
    {1736123160000LL, 1736123400000LL, 101.25, 101, 1, 0},
    {1736123580000LL, 1736123700000LL, 100.5, 102, 1, 0},
    {1736123880000LL, 1736123940000LL, 101.5, 100.75, 1, 1},
};

// expectation corrected (kStopLimitMag_rows, 40 of 40 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   3071528743731228315ull -> 12808919558760628660ull
//   7258651364804367398ull -> 1207316483359579033ull
//   7486144511252133314ull -> 4830106941319808031ull
//   1582607639630276898ull -> 4965860037920453570ull
//   3912714334640130040ull -> 10301076739317003984ull
//   10350628649333593370ull -> 5256643799696163180ull
//   9610457796405522380ull -> 14302673143861015358ull
//   4735484248822351545ull -> 4214308854242544203ull
//   17985911883515246495ull -> 6891214080784647525ull
//   14587725504656417022ull -> 5481401984989317420ull
//   40587139107344708ull -> 2546411593723775602ull
//   12157425683486083212ull -> 11076427695465349658ull
//   4136262435518675809ull -> 14604478211865393435ull
//   11923948587649127704ull -> 10441538420441446291ull
//   13066766983873203623ull -> 9745862717817944472ull
//   8430510488884878543ull -> 3957751200288033340ull
//   6931158058249735583ull -> 16843011464146841988ull
//   14505397101271770969ull -> 983828942721255196ull
//   16948063981554240553ull -> 1563629151903713594ull
//   13179954867246450550ull -> 15723186956609005545ull
//   9619984798924275601ull -> 5739085897818355485ull
//   14816304356761126499ull -> 9382643802341384815ull
//   11174136321351017738ull -> 14791511637869771770ull
//   6122884029815252026ull -> 1774502089806014214ull
//   14475038542198212591ull -> 3672468344554770191ull
//   17508186509023868181ull -> 9611641127168107032ull
//   285359117151198207ull -> 14264580171458586538ull
//   5315188594079872620ull -> 9855544492606044437ull
//   285202672923262241ull -> 15730083263451904656ull
//   5676964413179149390ull -> 12593008551508099760ull
//   2869944872687428965ull -> 6855239799020199958ull
//   1652949297200660571ull -> 15410865740197705624ull
//   13328843611540452942ull -> 14288704793329662189ull
//   1785519173963430584ull -> 8467709495611650267ull
//   3592185367493285450ull -> 8683892863737197217ull
//   6323004311246936550ull -> 9575704921439852097ull
//   1930593130638938736ull -> 7548568278893870919ull
//   17276396963236457283ull -> 4812994545824716644ull
//   10902255900630077694ull -> 7351634746524896233ull
//   3006349506749355420ull -> 15015254700001776595ull
// expectation corrected (v19-E, kStopLimitMag_rows, 40 of 40 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   12808919558760628660ull -> 1426413119368727739ull
//   1207316483359579033ull -> 11729774567786367716ull
//   4830106941319808031ull -> 14624533615872801840ull
//   4965860037920453570ull -> 10980939852557468738ull
//   10301076739317003984ull -> 4981677886757282483ull
//   5256643799696163180ull -> 7056152781752200150ull
//   14302673143861015358ull -> 222204814676374947ull
//   4214308854242544203ull -> 7215345707064513231ull
//   6891214080784647525ull -> 12693358032236412920ull
//   5481401984989317420ull -> 3137132028661502508ull
//   2546411593723775602ull -> 12339842671295629499ull
//   11076427695465349658ull -> 10538758131920122431ull
//   14604478211865393435ull -> 13851917084666735043ull
//   10441538420441446291ull -> 13459118864209887574ull
//   9745862717817944472ull -> 7161694476307517243ull
//   3957751200288033340ull -> 14598182289160501452ull
//   16843011464146841988ull -> 8829564412694297346ull
//   983828942721255196ull -> 15975791651856127856ull
//   1563629151903713594ull -> 4147368694952482168ull
//   15723186956609005545ull -> 17133134770366795920ull
//   5739085897818355485ull -> 9213888144382479152ull
//   9382643802341384815ull -> 4003628671076185617ull
//   14791511637869771770ull -> 10964253458288959331ull
//   1774502089806014214ull -> 6180087392066919114ull
//   3672468344554770191ull -> 9070734654282114891ull
//   9611641127168107032ull -> 4908014039146416295ull
//   14264580171458586538ull -> 4046059271741912732ull
//   9855544492606044437ull -> 2049239309329407579ull
//   15730083263451904656ull -> 15583436013445152044ull
//   12593008551508099760ull -> 3810321856910520893ull
//   6855239799020199958ull -> 5617254463899745622ull
//   15410865740197705624ull -> 16003016603397951197ull
//   14288704793329662189ull -> 8704315988438976676ull
//   8467709495611650267ull -> 1726448424722868060ull
//   8683892863737197217ull -> 16728008101916756905ull
//   9575704921439852097ull -> 8640413929875652210ull
//   7548568278893870919ull -> 15589794694703369101ull
//   4812994545824716644ull -> 10084593582520942716ull
//   7351634746524896233ull -> 7804430760616484748ull
//   15015254700001776595ull -> 12421926773651986280ull
constexpr std::uint64_t kStopLimitMag_rows[] = {
    1426413119368727739ull, 11729774567786367716ull, 14624533615872801840ull, 10980939852557468738ull,
    4981677886757282483ull, 7056152781752200150ull, 222204814676374947ull, 7215345707064513231ull,
    12693358032236412920ull, 3137132028661502508ull, 12339842671295629499ull, 10538758131920122431ull,
    13851917084666735043ull, 13459118864209887574ull, 7161694476307517243ull, 14598182289160501452ull,
    8829564412694297346ull, 15975791651856127856ull, 4147368694952482168ull, 17133134770366795920ull,
    9213888144382479152ull, 4003628671076185617ull, 10964253458288959331ull, 6180087392066919114ull,
    9070734654282114891ull, 4908014039146416295ull, 4046059271741912732ull, 2049239309329407579ull,
    15583436013445152044ull, 3810321856910520893ull, 5617254463899745622ull, 16003016603397951197ull,
    8704315988438976676ull, 1726448424722868060ull, 16728008101916756905ull, 8640413929875652210ull,
    15589794694703369101ull, 10084593582520942716ull, 7804430760616484748ull, 12421926773651986280ull,
};
constexpr std::uint64_t kStopLimitMag_at_bar[] = {
    4ull, 0ull, 13ull, 0ull,
    18ull, 0ull, 26ull, 0ull,
    31ull, 0ull, 40ull, 0ull,
    46ull, 0ull, 51ull, 0ull,
    59ull, 0ull, 64ull, 0ull,
    74ull, 0ull, 83ull, 0ull,
    88ull, 0ull, 96ull, 0ull,
    101ull, 0ull, 110ull, 0ull,
    116ull, 0ull, 122ull, 0ull,
    132ull, 0ull, 137ull, 0ull,
    146ull, 0ull, 152ull, 0ull,
    157ull, 0ull, 165ull, 0ull,
    170ull, 0ull, 180ull, 0ull,
    189ull, 0ull, 194ull, 0ull,
    200ull, 0ull, 207ull, 0ull,
    216ull, 0ull, 222ull, 0ull,
    227ull, 0ull, 235ull, 0ull,
    242ull, 0ull, 251ull, 0ull,
    257ull, 0ull, 262ull, 0ull,
    270ull, 0ull, 275ull, 0ull,
};
constexpr std::uint64_t kStopLimitMag_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    13ull, 0ull, 13ull, 0ull,
    18ull, 0ull, 18ull, 0ull,
    26ull, 0ull, 26ull, 0ull,
    31ull, 0ull, 31ull, 0ull,
    40ull, 0ull, 40ull, 0ull,
    46ull, 0ull, 46ull, 0ull,
    51ull, 0ull, 51ull, 0ull,
    59ull, 0ull, 59ull, 0ull,
    64ull, 0ull, 64ull, 0ull,
    74ull, 0ull, 74ull, 0ull,
    83ull, 0ull, 83ull, 0ull,
    88ull, 0ull, 88ull, 0ull,
    96ull, 0ull, 96ull, 0ull,
    101ull, 0ull, 101ull, 0ull,
    110ull, 0ull, 110ull, 0ull,
    116ull, 0ull, 116ull, 0ull,
    122ull, 0ull, 122ull, 0ull,
    132ull, 0ull, 132ull, 0ull,
    137ull, 0ull, 137ull, 0ull,
    146ull, 0ull, 146ull, 0ull,
    152ull, 0ull, 152ull, 0ull,
    157ull, 0ull, 157ull, 0ull,
    165ull, 0ull, 165ull, 0ull,
    170ull, 0ull, 170ull, 0ull,
    180ull, 0ull, 180ull, 0ull,
    189ull, 0ull, 189ull, 0ull,
    194ull, 0ull, 194ull, 0ull,
    200ull, 0ull, 200ull, 0ull,
    207ull, 0ull, 207ull, 0ull,
    216ull, 0ull, 216ull, 0ull,
    222ull, 0ull, 222ull, 0ull,
    227ull, 0ull, 227ull, 0ull,
    235ull, 0ull, 235ull, 0ull,
    242ull, 0ull, 242ull, 0ull,
    251ull, 0ull, 251ull, 0ull,
    257ull, 0ull, 257ull, 0ull,
    262ull, 0ull, 262ull, 0ull,
    270ull, 0ull, 270ull, 0ull,
    275ull, 0ull, 275ull, 0ull,
    275ull, 0ull,
};
// expectation corrected: kStopLimitMag_final 3006349506749355420ull -> 15015254700001776595ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kStopLimitMag_final 15015254700001776595ull -> 12421926773651986280ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kStopLimitMag_final = 12421926773651986280ull;
constexpr Trade kStopLimitMag_trades[] = {
    {1736121660000LL, 1736121780000LL, 100.25, 101.5, 1, 0},
    {1736121660000LL, 1736121900000LL, 100.25, 102, 1, 0},
    {1736122080000LL, 1736122200000LL, 100.5, 101.25, 1, 0},
    {1736122260000LL, 1736122380000LL, 101.5, 101.75, 1, 0},
    {1736122260000LL, 1736122500000LL, 101.5, 100.5, 1, 0},
    {1736122680000LL, 1736122680000LL, 101, 101, 1, 0},
    {1736122680000LL, 1736122800000LL, 101, 102.5, 1, 0},
    {1736122980000LL, 1736123100000LL, 101, 100.75, 1, 0},
    {1736123160000LL, 1736123340000LL, 101, 101.5, 1, 0},
    {1736123160000LL, 1736123400000LL, 101, 101, 1, 0},
    {1736123580000LL, 1736123700000LL, 100.5, 102, 1, 0},
    {1736123880000LL, 1736123940000LL, 101.5, 100.75, 1, 1},
};

// expectation corrected (kQuiet_rows, 16 of 16 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   7724584198093564595ull -> 16959315319659428706ull
//   17550962243433917936ull -> 6593833943716572457ull
//   9541920816613443162ull -> 2703197803945279413ull
//   12710625101738735578ull -> 15190772270010299065ull
//   7320008065855094423ull -> 6464070309819418391ull
//   613337425736277998ull -> 2641424632348884334ull
//   3296452229754818486ull -> 11893536079922902070ull
//   6283008522085308740ull -> 2755832076334792388ull
//   11889563166378766795ull -> 11960057815978144331ull
//   17204044324582861992ull -> 10577829118282217256ull
//   8038452434220172750ull -> 4559471071286615118ull
//   5335291432203819963ull -> 13616694969642505019ull
//   238567153239955510ull -> 10908038571276323510ull
//   280037730591905536ull -> 4265099920160034432ull
//   8600631936408560883ull -> 11117851636219666803ull
//   6139548852501668824ull -> 15339934536417004376ull
// expectation corrected (v19-E, kQuiet_rows, 16 of 16 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   16959315319659428706ull -> 2251190330680026869ull
//   6593833943716572457ull -> 15910380323894423478ull
//   2703197803945279413ull -> 14880679048257127815ull
//   15190772270010299065ull -> 16864304942012172441ull
//   6464070309819418391ull -> 2793384994174118460ull
//   2641424632348884334ull -> 7368859546216696052ull
//   11893536079922902070ull -> 7236161865638212397ull
//   2755832076334792388ull -> 9180718770365304561ull
//   11960057815978144331ull -> 534753123503696323ull
//   10577829118282217256ull -> 257995351140209626ull
//   4559471071286615118ull -> 4505359601117927185ull
//   13616694969642505019ull -> 13588639401292320613ull
//   10908038571276323510ull -> 316557109720746540ull
//   4265099920160034432ull -> 117536285131979531ull
//   11117851636219666803ull -> 15011555328205275466ull
//   15339934536417004376ull -> 10880804929896122760ull
constexpr std::uint64_t kQuiet_rows[] = {
    2251190330680026869ull, 15910380323894423478ull, 14880679048257127815ull, 16864304942012172441ull,
    2793384994174118460ull, 7368859546216696052ull, 7236161865638212397ull, 9180718770365304561ull,
    534753123503696323ull, 257995351140209626ull, 4505359601117927185ull, 13588639401292320613ull,
    316557109720746540ull, 117536285131979531ull, 15011555328205275466ull, 10880804929896122760ull,
};
constexpr std::uint64_t kQuiet_at_bar[] = {
    4ull, 0ull, 9ull, 0ull,
    17ull, 14ull, 22ull, 14ull,
    31ull, 27ull, 36ull, 27ull,
    41ull, 27ull, 46ull, 27ull,
    51ull, 27ull, 56ull, 27ull,
    61ull, 27ull, 66ull, 27ull,
    71ull, 27ull, 76ull, 27ull,
    81ull, 27ull, 86ull, 27ull,
};
constexpr std::uint64_t kQuiet_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    9ull, 0ull, 9ull, 0ull,
    17ull, 14ull, 17ull, 14ull,
    22ull, 14ull, 22ull, 14ull,
    31ull, 27ull, 31ull, 27ull,
    36ull, 27ull, 36ull, 27ull,
    41ull, 27ull, 41ull, 27ull,
    46ull, 27ull, 46ull, 27ull,
    51ull, 27ull, 51ull, 27ull,
    56ull, 27ull, 56ull, 27ull,
    61ull, 27ull, 61ull, 27ull,
    66ull, 27ull, 66ull, 27ull,
    71ull, 27ull, 71ull, 27ull,
    76ull, 27ull, 76ull, 27ull,
    81ull, 27ull, 81ull, 27ull,
    86ull, 27ull, 86ull, 27ull,
    86ull, 27ull,
};
// expectation corrected: kQuiet_final 6139548852501668824ull -> 15339934536417004376ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kQuiet_final 15339934536417004376ull -> 10880804929896122760ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kQuiet_final = 10880804929896122760ull;
constexpr Trade kQuiet_trades[] = {
    {1736121720000LL, 1736121840000LL, 101.5, 102.25, 2, 0},
};

// expectation corrected (kQuietMag_rows, 16 of 16 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move:
//   5733365443022361725ull -> 1611239520478642856ull
//   10765881105436603986ull -> 15670924759075988135ull
//   10309005793693845566ull -> 8953422413855088901ull
//   4130036604897460242ull -> 8602069986686684101ull
//   282822265959139076ull -> 17678399590938719620ull
//   17106921954780902873ull -> 15811832608659800921ull
//   14287768468197425669ull -> 17460480541635931781ull
//   16598702599498466611ull -> 2259743306748045491ull
//   7476876757385613240ull -> 8947015072418775096ull
//   17399942887626154327ull -> 6496064291649989079ull
//   1540537395942304357ull -> 4703247870626149093ull
//   275319011854108248ull -> 17365430348831147992ull
//   10464635064168676601ull -> 7048118702907062137ull
//   16403636294463212927ull -> 14660228885507672831ull
//   8919487187323291724ull -> 17539868185903775948ull
//   12364294267467016655ull -> 15979450874488009551ull
// expectation corrected (v19-E, kQuietMag_rows, 16 of 16 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   1611239520478642856ull -> 1771071767131182639ull
//   15670924759075988135ull -> 11511402876228497534ull
//   8953422413855088901ull -> 13505881934138930069ull
//   8602069986686684101ull -> 17435988220685787619ull
//   17678399590938719620ull -> 3320320633831017015ull
//   15811832608659800921ull -> 14967137603421812042ull
//   17460480541635931781ull -> 8853087616657613471ull
//   2259743306748045491ull -> 18314669170025281607ull
//   8947015072418775096ull -> 10755318122153801645ull
//   6496064291649989079ull -> 10163627219265018624ull
//   4703247870626149093ull -> 114643937275360575ull
//   17365430348831147992ull -> 598916062260217647ull
//   7048118702907062137ull -> 13904582099442899970ull
//   14660228885507672831ull -> 10276732342521370229ull
//   17539868185903775948ull -> 7149263525418787648ull
//   15979450874488009551ull -> 18272199307858808366ull
constexpr std::uint64_t kQuietMag_rows[] = {
    1771071767131182639ull, 11511402876228497534ull, 13505881934138930069ull, 17435988220685787619ull,
    3320320633831017015ull, 14967137603421812042ull, 8853087616657613471ull, 18314669170025281607ull,
    10755318122153801645ull, 10163627219265018624ull, 114643937275360575ull, 598916062260217647ull,
    13904582099442899970ull, 10276732342521370229ull, 7149263525418787648ull, 18272199307858808366ull,
};
constexpr std::uint64_t kQuietMag_at_bar[] = {
    4ull, 0ull, 9ull, 0ull,
    17ull, 0ull, 22ull, 0ull,
    31ull, 0ull, 36ull, 0ull,
    41ull, 0ull, 46ull, 0ull,
    51ull, 0ull, 56ull, 0ull,
    61ull, 0ull, 66ull, 0ull,
    71ull, 0ull, 76ull, 0ull,
    81ull, 0ull, 86ull, 0ull,
};
constexpr std::uint64_t kQuietMag_folded[] = {
    4ull, 0ull, 4ull, 0ull,
    9ull, 0ull, 9ull, 0ull,
    17ull, 0ull, 17ull, 0ull,
    22ull, 0ull, 22ull, 0ull,
    31ull, 0ull, 31ull, 0ull,
    36ull, 0ull, 36ull, 0ull,
    41ull, 0ull, 41ull, 0ull,
    46ull, 0ull, 46ull, 0ull,
    51ull, 0ull, 51ull, 0ull,
    56ull, 0ull, 56ull, 0ull,
    61ull, 0ull, 61ull, 0ull,
    66ull, 0ull, 66ull, 0ull,
    71ull, 0ull, 71ull, 0ull,
    76ull, 0ull, 76ull, 0ull,
    81ull, 0ull, 81ull, 0ull,
    86ull, 0ull, 86ull, 0ull,
    86ull, 0ull,
};
// expectation corrected: kQuietMag_final 12364294267467016655ull -> 15979450874488009551ull, because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19); receipt cursors and trades did not move.
// expectation corrected (v19-E): kQuietMag_final 15979450874488009551ull -> 18272199307858808366ull, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); the receipt bars, folded counts and trades did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows).
constexpr std::uint64_t kQuietMag_final = 18272199307858808366ull;
constexpr Trade kQuietMag_trades[] = {
    {1736121720000LL, 1736121840000LL, 101.5, 102.25, 2, 0},
};

// P4_PINNED_DATA_END

bool operator==(const Trade& a, const Trade& b) {
    return a.entry_time == b.entry_time && a.exit_time == b.exit_time
        && a.entry_price == b.entry_price && a.exit_price == b.exit_price
        && a.qty == b.qty && a.open_at_end == b.open_at_end;
}

template <std::size_t N>
std::vector<std::uint64_t> list(const std::uint64_t (&values)[N]) {
    return {values, values + N};
}

template <std::size_t N>
std::vector<Trade> trades(const Trade (&values)[N]) {
    std::vector<Trade> out;
    for (const auto& t : values)
        if (t.open_at_end >= 0) out.push_back(t);
    return out;
}

struct Expected {
    Scenario scenario;
    bool magnifier;
    std::vector<std::uint64_t> rows;
    std::vector<std::uint64_t> at_bar;
    std::vector<std::uint64_t> folded;
    std::uint64_t final_hash;
    std::vector<Trade> trades;
};

#define P4_EXPECTED(name, scenario, magnifier)                                 \
    Expected{scenario, magnifier, list(k##name##_rows), list(k##name##_at_bar), \
             list(k##name##_folded), k##name##_final, trades(k##name##_trades)}

std::vector<Expected> expected() {
    return {
        P4_EXPECTED(Declined, Scenario::Declined, false),
        P4_EXPECTED(DeclinedMag, Scenario::Declined, true),
        P4_EXPECTED(Revive, Scenario::Revive, false),
        P4_EXPECTED(ReviveMag, Scenario::Revive, true),
        P4_EXPECTED(Cascade, Scenario::Cascade, false),
        P4_EXPECTED(CascadeMag, Scenario::Cascade, true),
        P4_EXPECTED(Brackets, Scenario::Brackets, false),
        P4_EXPECTED(BracketsMag, Scenario::Brackets, true),
        P4_EXPECTED(Groups, Scenario::Groups, false),
        P4_EXPECTED(GroupsMag, Scenario::Groups, true),
        P4_EXPECTED(StopLimit, Scenario::StopLimit, false),
        P4_EXPECTED(StopLimitMag, Scenario::StopLimit, true),
        P4_EXPECTED(Quiet, Scenario::Quiet, false),
        P4_EXPECTED(QuietMag, Scenario::Quiet, true),
    };
}

void check_pinned(const Expected& want) {
    const Observed got = observe(want.scenario, want.magnifier);
    const auto before = failures;
    CHECK(got.rows == want.rows);
    CHECK(got.at_bar == want.at_bar);
    CHECK(got.folded == want.folded);
    CHECK(got.final_hash == want.final_hash);
    CHECK(got.trades == want.trades);
    if (failures != before) {
        std::fprintf(stderr, "  scenario %s, magnifier %s\n", name_of(want.scenario),
                     want.magnifier ? "on" : "off");
    }
}

// A read over driver points and account rows only allocates nothing. The
// run is over; the read is taken again from the last command, and it must
// end on the high water every row above that command reaches.
void a_commandless_read_allocates_nothing(bool magnifier) {
    WitnessHost host(Scenario::Quiet);
    // The read below walks the run's whole event record once it has ended.
    host.fixture_retain_all_events();
    run(host, Scenario::Quiet, magnifier);
    std::uint64_t last_command = 0;
    std::uint64_t high_water = 0;
    for (const auto& row : host.native_events(0)) {
        high_water = std::max(high_water, row.ordinal);
        if (row.command) last_command = row.ordinal;
    }
    CHECK(last_command > 0);
    CHECK(high_water > last_command);
    CHECK(host.receipt_cursor() > last_command);
    const std::uint64_t terminal = host.terminal_cursor();
    host.rewind_receipt_cursor(last_command);
    std::size_t allocations = global_allocation::allocations;
    host.observe_receipts();
    allocations = global_allocation::allocations - allocations;
    if (allocations != 0) {
        std::fprintf(stderr, "  commandless receipt read (magnifier %s): %zu allocations\n",
                     magnifier ? "on" : "off", allocations);
    }
    CHECK(allocations == 0);
    CHECK(host.receipt_cursor() == high_water);
    CHECK(host.terminal_cursor() == terminal);
}
#endif

}  // namespace

int main() {
#ifdef PINEFORGE_P4_HARVEST
    std::printf("// P4_PINNED_DATA_BEGIN\n");
    for (const Scenario scenario : kScenarios) {
        emit(scenario, false, observe(scenario, false));
        emit(scenario, true, observe(scenario, true));
    }
    std::printf("// P4_PINNED_DATA_END\n");
    return 0;
#else
    for (const auto& want : expected()) check_pinned(want);
    a_commandless_read_allocates_nothing(false);
    a_commandless_read_allocates_nothing(true);
    if (failures == 0) {
        std::printf("test_adapter_receipts_in_place: ok (%d checks)\n", checks);
        return 0;
    }
    std::fprintf(stderr, "test_adapter_receipts_in_place: %d of %d checks failed\n",
                 failures, checks);
    return 1;
#endif
}
