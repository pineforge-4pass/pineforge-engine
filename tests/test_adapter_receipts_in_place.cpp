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
constexpr std::uint64_t kDeclined_rows[] = {
    4285364442943983342ull, 5064739043285013209ull, 14226248201720924480ull, 11756561249042112686ull,
    7502546032116966765ull, 9343248170671305998ull, 16170761826422571350ull, 17156268346509373136ull,
    7683160624530869717ull, 1667123404342696863ull,
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
constexpr std::uint64_t kDeclined_final = 1667123404342696863ull;
constexpr Trade kDeclined_trades[] = {
    {1736121660000LL, 1736121960000LL, 100, 95, 100, 0},
};

constexpr std::uint64_t kDeclinedMag_rows[] = {
    15827241547792344980ull, 1131814333350244112ull, 16157453329770597182ull, 13919762176745114404ull,
    9610884682524537723ull, 16385497926780059888ull, 11525863595680394547ull, 1510992411965426849ull,
    14098169462848375672ull, 16277588925252464314ull,
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
constexpr std::uint64_t kDeclinedMag_final = 16277588925252464314ull;
constexpr Trade kDeclinedMag_trades[] = {
    {1736121660000LL, 1736121960000LL, 100, 95, 100, 0},
};

constexpr std::uint64_t kRevive_rows[] = {
    6350627700968043432ull, 7893630477953678062ull, 6286632809044419740ull, 11489615726470058275ull,
    5071096704365483852ull, 10124416207050257042ull, 3480095526769689953ull,
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
constexpr std::uint64_t kRevive_final = 3480095526769689953ull;
constexpr Trade kRevive_trades[] = {
    {1736121660000LL, 1736121780000LL, 100, 170, 47.058823529411768, 0},
    {1736121660000LL, 1736121840000LL, 100, 180, 52.941176470588232, 0},
};

constexpr std::uint64_t kReviveMag_rows[] = {
    1579493378216782912ull, 7511772062963861370ull, 15379415014274107418ull, 16105510747798700943ull,
    11290395518254980142ull, 1443155722669931577ull, 11170248427254383026ull,
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
constexpr std::uint64_t kReviveMag_final = 11170248427254383026ull;
constexpr Trade kReviveMag_trades[] = {
    {1736121660000LL, 1736121840000LL, 100, 175, 100, 0},
};

constexpr std::uint64_t kCascade_rows[] = {
    11803825725282039008ull, 11121191306941517363ull, 3178279434697480731ull, 7425460857334141232ull,
    8350079040744008319ull, 9723365308680356972ull, 17244746764465919736ull,
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
constexpr std::uint64_t kCascade_final = 17244746764465919736ull;
constexpr Trade kCascade_trades[] = {
    {1736121660000LL, 1736121780000LL, 100, 170, 47.058823529411768, 0},
    {1736121660000LL, 1736121780000LL, 100, 170, 52.941176470588232, 0},
};

constexpr std::uint64_t kCascadeMag_rows[] = {
    15795341388107798312ull, 4633146533710855253ull, 10768521223239694579ull, 15472347828572140999ull,
    16884913969937812814ull, 12305957632002004150ull, 12442414818770313178ull,
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
constexpr std::uint64_t kCascadeMag_final = 12442414818770313178ull;
constexpr Trade kCascadeMag_trades[] = {
    {1736121660000LL, 1736121840000LL, 100, 168, 19.047619047619047, 0},
    {1736121660000LL, 1736121840000LL, 100, 168, 80.952380952380949, 0},
};

constexpr std::uint64_t kBrackets_rows[] = {
    14866027288923674590ull, 16649853781238903429ull, 13190095744217365753ull, 10502269067036593532ull,
    18381143185443891561ull, 1735915102031019572ull, 10395586145912073704ull, 2679523764946147230ull,
    18384433671636462098ull, 18211235479487566787ull, 18277897802945420404ull, 17395674248469413919ull,
    13220479791033372227ull, 15569212298721506404ull, 8779802303491361775ull, 5372534098409924662ull,
    15552749709575985492ull, 13902135963118532762ull, 1437248562486350733ull, 10518682552051823326ull,
    14625840823528391271ull, 6916250278448210655ull, 16347660827743144968ull, 5193341365299488091ull,
    12591925847622054552ull, 5455453127300802186ull, 15337733870115412176ull, 13393536876314999769ull,
    14506760582926131442ull, 6398254635611204925ull, 16786661275526125283ull, 11680808028246450544ull,
    10881585127984591333ull, 10699952175155992651ull, 3957098159049250088ull, 12537604784772314763ull,
    14032309735782374896ull, 13456059580862079650ull, 7207516763549747550ull, 12275462523833622145ull,
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
constexpr std::uint64_t kBrackets_final = 12275462523833622145ull;
constexpr Trade kBrackets_trades[] = {
    {1736121660000LL, 1736121660000LL, 100.75, 101, 2, 0},
    {1736122020000LL, 1736122260000LL, 100.75, 102, 2, 0},
    {1736122740000LL, 1736122740000LL, 101.75, 102, 2, 0},
    {1736123100000LL, 1736123100000LL, 100.75, 101, 2, 0},
    {1736123820000LL, 1736123940000LL, 101.75, 100.5, 2, 0},
};

constexpr std::uint64_t kBracketsMag_rows[] = {
    14925296382428985648ull, 7344905789884533150ull, 16090565159374661598ull, 11225360465726061199ull,
    4423772731596842302ull, 1864118206373791711ull, 16170709559648037869ull, 481673215253245186ull,
    14209727281670483637ull, 5231187342479144464ull, 12956579884983977310ull, 16877300195273226901ull,
    3518124402729136401ull, 18194591487925347118ull, 11275175479190670649ull, 1727531436455231448ull,
    2812906101421470624ull, 14507886661839533642ull, 13272932003274552705ull, 1254610278521633748ull,
    5296589554740508017ull, 5020844913341177189ull, 6534636244690225506ull, 18437577580270433421ull,
    13673021230426178520ull, 10160723947616790626ull, 600322673604262576ull, 11221611113249085965ull,
    2328355405364402798ull, 11300656303009114157ull, 16771700425934511859ull, 11126451407672334260ull,
    14182501097978637485ull, 4654667524987087133ull, 13979336787298677508ull, 5593389497923022859ull,
    2748819722934504052ull, 3185167942597426079ull, 6037389201975695465ull, 4480275903615486608ull,
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
constexpr std::uint64_t kBracketsMag_final = 4480275903615486608ull;
constexpr Trade kBracketsMag_trades[] = {
    {1736121660000LL, 1736121660000LL, 100.75, 101.25, 2, 0},
    {1736122020000LL, 1736122260000LL, 100.75, 102, 2, 0},
    {1736122740000LL, 1736122740000LL, 101.75, 102.25, 2, 0},
    {1736123100000LL, 1736123100000LL, 100.75, 101.25, 2, 0},
    {1736123820000LL, 1736123940000LL, 101.75, 100.5, 2, 0},
};

constexpr std::uint64_t kGroups_rows[] = {
    7724584198093564595ull, 14037535911552261334ull, 14964184649618471106ull, 15837640502650876441ull,
    5841811833025652711ull, 6880597703884275978ull, 4022005563316796046ull, 11646411279043485842ull,
    2025125997917000074ull, 14963035782174266194ull, 13703469485999267191ull, 11785310893319292714ull,
    5562700109292854673ull, 13051181478567279819ull, 15362637042632761092ull, 11337152299125080166ull,
    14366095805963646679ull, 13006999377430155062ull, 18251237808884358198ull, 3367226118791643879ull,
    10112133094528063767ull, 5632786340700576450ull, 9542058346449055539ull, 5336767761041012898ull,
    5050327092814624473ull, 9259344525213473815ull, 14339458378944358911ull, 11531371741958840313ull,
    6841871301419080046ull, 16333070905054532344ull, 15431932348706501099ull, 7926864697956809263ull,
    175881041745163829ull, 1160678453166274165ull, 624762104954538763ull, 9950397847729605829ull,
    7375119601134904582ull, 5972030537660888252ull, 10485455422255788959ull, 11093578271132015941ull,
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
constexpr std::uint64_t kGroups_final = 11093578271132015941ull;
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

constexpr std::uint64_t kGroupsMag_rows[] = {
    5733365443022361725ull, 1770031795907811498ull, 6470203284857417282ull, 13680705116515132995ull,
    16280306007782621573ull, 17886876073090196864ull, 13570091299407770576ull, 17049172465269546805ull,
    6168640773089004757ull, 4304075059045850359ull, 13725508340902668646ull, 3951890106122286393ull,
    9085666311990288374ull, 12954585096737187612ull, 6795169276410185767ull, 12139316376347823538ull,
    6087488785891629379ull, 3648533416536772720ull, 16869591150172377173ull, 14514487283608884298ull,
    12777372886064072601ull, 8639486342548277404ull, 14296087827019584825ull, 7513667181080739246ull,
    16005201315071742289ull, 7683997310820400007ull, 274527616443811739ull, 13774001486483371441ull,
    2038523230773680902ull, 4394512243199420690ull, 6875720906349168873ull, 3027886066460029894ull,
    12678959196294236002ull, 11136522415607694552ull, 2000687809818107336ull, 4060242198356548732ull,
    14187333568294550129ull, 12558284470601575633ull, 13092014301223548584ull, 8471333527175518852ull,
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
constexpr std::uint64_t kGroupsMag_final = 8471333527175518852ull;
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

constexpr std::uint64_t kStopLimit_rows[] = {
    2951874735862626799ull, 11001154575678754787ull, 10191383546300744753ull, 14335067750512523113ull,
    14634254702333820496ull, 13727563301137820252ull, 7732167413016711558ull, 23989540955952803ull,
    17868341505123005444ull, 8901216489246200621ull, 7235870513229523578ull, 14347351966641657743ull,
    15514857036218605458ull, 3292206687046947377ull, 11664433890693965409ull, 12016486425273185683ull,
    7858597352620855107ull, 7409416796933497235ull, 15475869289132129140ull, 6794288551174200691ull,
    5047559130542054898ull, 4592577991936859864ull, 4145923563897710011ull, 14084652476072856286ull,
    4367276615515885720ull, 4103383789594518773ull, 4078016538705775748ull, 12900085140413569457ull,
    281955135076910959ull, 1219123596768957497ull, 2593395659259851147ull, 13366497277076464801ull,
    14903972582846578408ull, 4655963223934593536ull, 14602679593300513726ull, 7172060859019788156ull,
    10076308138673754946ull, 2251278248701969261ull, 7350541109135979421ull, 2398349164106856788ull,
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
constexpr std::uint64_t kStopLimit_final = 2398349164106856788ull;
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

constexpr std::uint64_t kStopLimitMag_rows[] = {
    3071528743731228315ull, 7258651364804367398ull, 7486144511252133314ull, 1582607639630276898ull,
    3912714334640130040ull, 10350628649333593370ull, 9610457796405522380ull, 4735484248822351545ull,
    17985911883515246495ull, 14587725504656417022ull, 40587139107344708ull, 12157425683486083212ull,
    4136262435518675809ull, 11923948587649127704ull, 13066766983873203623ull, 8430510488884878543ull,
    6931158058249735583ull, 14505397101271770969ull, 16948063981554240553ull, 13179954867246450550ull,
    9619984798924275601ull, 14816304356761126499ull, 11174136321351017738ull, 6122884029815252026ull,
    14475038542198212591ull, 17508186509023868181ull, 285359117151198207ull, 5315188594079872620ull,
    285202672923262241ull, 5676964413179149390ull, 2869944872687428965ull, 1652949297200660571ull,
    13328843611540452942ull, 1785519173963430584ull, 3592185367493285450ull, 6323004311246936550ull,
    1930593130638938736ull, 17276396963236457283ull, 10902255900630077694ull, 3006349506749355420ull,
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
constexpr std::uint64_t kStopLimitMag_final = 3006349506749355420ull;
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

constexpr std::uint64_t kQuiet_rows[] = {
    7724584198093564595ull, 17550962243433917936ull, 9541920816613443162ull, 12710625101738735578ull,
    7320008065855094423ull, 613337425736277998ull, 3296452229754818486ull, 6283008522085308740ull,
    11889563166378766795ull, 17204044324582861992ull, 8038452434220172750ull, 5335291432203819963ull,
    238567153239955510ull, 280037730591905536ull, 8600631936408560883ull, 6139548852501668824ull,
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
constexpr std::uint64_t kQuiet_final = 6139548852501668824ull;
constexpr Trade kQuiet_trades[] = {
    {1736121720000LL, 1736121840000LL, 101.5, 102.25, 2, 0},
};

constexpr std::uint64_t kQuietMag_rows[] = {
    5733365443022361725ull, 10765881105436603986ull, 10309005793693845566ull, 4130036604897460242ull,
    282822265959139076ull, 17106921954780902873ull, 14287768468197425669ull, 16598702599498466611ull,
    7476876757385613240ull, 17399942887626154327ull, 1540537395942304357ull, 275319011854108248ull,
    10464635064168676601ull, 16403636294463212927ull, 8919487187323291724ull, 12364294267467016655ull,
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
constexpr std::uint64_t kQuietMag_final = 12364294267467016655ull;
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
