// R5 lane PERF-P7: the values a Pine run reaches through the lookups the
// lane indexes, pinned before the indexes existed.
//
// Four places in the Pine adapter and the request core answered a question
// by walking history that only grows:
//   - purge_brackets_after_applied_reversal and bracket_belongs_to_reversal
//     walked every origin a cohort ever had, looking for an opening on the
//     side the reversal closed;
//   - consume_closed_trade_rows walked every origin of the trade's cohort to
//     find the few that still carry units;
//   - exit() asked whether an origin's leg was consumed by walking every leg
//     its (exit id, from_entry) family ever placed;
//   - WorkingRequestCore::definition_for scanned the command history
//     backwards for a request that is no longer working, once per link of a
//     replace chain, at every cohort_add.
// An index must answer each of those exactly as the walk did. So this
// witness is data: every recorded broker-state row, a read after every
// command the scenario issues, the final scalar and every trade, for four
// scenarios built to drive those lookups -- reversals between cohorts with
// brackets live on both sides and a cohort that opens on both sides
// (Flip); two quantity brackets per origin re-issued every bar, a leg issued
// while its parent is flat and bound when the parent fills, a bracket
// cancelled and re-issued (Brackets); a far stop re-issued for the whole run
// and a limit re-priced every bar, both joined to their cohort at every
// re-issue (Chains); and a leveraged position whose brackets live through
// margin calls (Margin). Observed on engine f71cd820 (the lane's base) and
// pinned here; it says nothing about cost (test_adapter_lookup_index_scaling
// does).
//
// Portability. As in test_adapter_recording_hash_witness, the host overrides
// the projection to fold one fixed execution hash, so what remains is the
// kernel's broker state and the whole source extension; every price is a
// whole number of quarter ticks, so every hashed double is exact.
//
// Provenance of the pinned data: this TU, compiled unchanged against the
// f71cd820 library with -DPINEFORGE_P7_HARVEST (which prints the observed
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

constexpr std::uint64_t kProbeExecutionHash = 0x5eed1234abcd00f7ull;
constexpr std::int64_t T = 1736121600000LL;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

enum class Scenario { Flip, Brackets, Chains, Margin };

const char* name_of(Scenario scenario) {
    switch (scenario) {
    case Scenario::Flip: return "Flip";
    case Scenario::Brackets: return "Brackets";
    case Scenario::Chains: return "Chains";
    case Scenario::Margin: return "Margin";
    }
    return "?";
}

int bars_of(Scenario scenario) {
    switch (scenario) {
    case Scenario::Flip: return 96;
    case Scenario::Brackets: return 120;
    case Scenario::Chains: return 160;
    case Scenario::Margin: return 96;
    }
    return 0;
}

long triangle(int index, int period) {
    const int phase = index % (2 * period);
    return phase < period ? phase : 2 * period - phase;
}

// Prices in quarter ticks: three triangle waves (whole numbers, so every
// price is an exact binary fraction), and for Margin a slide of a fifth of
// the price and a recovery.
long price_ticks(Scenario scenario, int index) {
    long ticks = 400 + 2 * triangle(index, 9) + 3 * triangle(index, 23) - triangle(index, 4);
    if (scenario == Scenario::Margin) {
        if (index >= 40 && index < 56) ticks -= 6 * (index - 40);
        else if (index >= 56 && index < 72) ticks -= 6 * (72 - index);
    }
    return ticks;
}

std::vector<Bar> feed(Scenario scenario) {
    std::vector<Bar> bars;
    const int count = bars_of(scenario);
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double open = 0.25 * static_cast<double>(price_ticks(scenario, i));
        const double close = 0.25 * static_cast<double>(price_ticks(scenario, i + 1));
        const double high = (open > close ? open : close) + 0.5;
        const double low = (open < close ? open : close) - 0.5;
        bars.push_back({open, high, low, close, 1.0,
                        T + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

// A generated-strategy-shaped source host: the body issues Pine commands and
// reads the hash after each command batch.
class WitnessHost final : public source::PineStrategyHost {
public:
    WitnessHost(Scenario scenario, bool recording) : scenario_(scenario) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = scenario == Scenario::Flip ? 2 : 1;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.5;
        if (scenario == Scenario::Margin) {
            config.initial_capital = 10000.0;
            config.default_qty_value = 300.0;
            config.margin_long = 25.0;
            config.margin_short = 25.0;
        }
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
        case Scenario::Flip: flip(i, bar); break;
        case Scenario::Brackets: brackets(i, bar); break;
        case Scenario::Chains: chains(i, bar); break;
        case Scenario::Margin: margin(i, bar); break;
        }
        reads.push_back(broker_state_hash());
    }

private:
    // Reversals between two cohorts every three bars, a pyramiding add, a
    // cohort that opens long and short, brackets live on the side a reversal
    // closes (a dynamic one on L, per-origin quantity legs on S), partial
    // closes and raw orders.
    void flip(int i, const Bar& bar) {
        const double position = live_position_size();
        if (i % 6 == 1) strategy_entry("L", true);
        if (i % 6 == 4) strategy_entry("S", false);
        if (i % 12 == 2 && position > 0.0) strategy_entry("L2", true);
        if (i % 18 == 9) strategy_entry("B", true);
        if (i % 18 == 15) strategy_entry("B", false);
        if (position > 0.0) strategy_exit("xl", "L", bar.close + 2.0, bar.close - 2.0);
        if (position < 0.0) {
            strategy_exit("xs", "S", bar.close - 2.0, bar.close + 2.0, kNa, kNa, kNa, 100.0,
                          {}, 1.0);
        }
        if (i % 10 == 7) strategy_close("L", {}, 1.0);
        if (i % 14 == 11) strategy_order("o", true, 1.0);
        if (i % 14 == 12) strategy_order("o", false, 1.0);
    }

    // Two quantity brackets per origin, each in its own group, re-issued at
    // moving levels every bar (the oca-multi-bracket shape); a pyramiding
    // re-entry after the first bracket fills; a leg issued before its parent
    // exists (origin zero, bound when the parent fills); a bracket cancelled
    // and re-issued; a flat reset.
    void brackets(int i, const Bar& bar) {
        const double position = live_position_size();
        if (position == 0.0 && i % 9 == 3) {
            strategy_exit("X_P", "L", bar.close + 3.0, bar.close - 3.0, kNa, kNa, kNa, 100.0,
                          {}, 1.0, "GRP_P");
        }
        if (position == 0.0 && i % 9 == 4) strategy_entry("L", true, kNa, kNa, 2.0);
        if (position > 0.0) {
            const double width = 1.0 + 0.25 * static_cast<double>(i % 4);
            strategy_exit("X_A", "L", bar.close + width, bar.close - width, kNa, kNa, kNa,
                          100.0, {}, 1.0, "GRP_A");
            strategy_exit("X_B", "L", bar.close + 2.0 * width, bar.close - 2.0 * width, kNa,
                          kNa, kNa, 100.0, {}, 1.0, "GRP_B");
        }
        if (i % 25 == 17) strategy_exit_cancel_bracket("X_B", "L");
        if (i % 40 == 39) strategy_close_all();
    }

    // A far stop re-issued every sixteen bars for the whole run (it never
    // fills, so its replace chain only grows), a near stop re-issued for four
    // bars at a time, a limit re-priced every bar for sixty bars, closes, a
    // cancel, and a dynamic bracket bound to the near stop's cohort.
    void chains(int i, const Bar& bar) {
        const double position = live_position_size();
        if (i % 16 == 0) strategy_entry("SE", false, kNa, bar.low * 0.5);
        if (position == 0.0 && i % 16 >= 1 && i % 16 <= 4)
            strategy_entry("LE", true, kNa, bar.high + 0.25);
        if (i >= 60 && i < 120) strategy_entry("LB", true, bar.close - 3.0);
        if (position > 0.0) strategy_exit("xle", "LE", bar.close + 4.0, bar.close - 4.0);
        if (position > 0.0 && i % 16 == 10) strategy_close("LE");
        if (i % 16 == 12) strategy_close("LB");
        if (i == 130) strategy_cancel("SE");
    }

    // A leveraged long whose brackets live through margin calls in the
    // slide, a reversal attempt at the bottom, and smaller re-entries the
    // remaining equity can carry.
    void margin(int i, const Bar& bar) {
        const double position = live_position_size();
        if (i == 2) strategy_entry("L", true);
        if (i == 76 || i == 86) strategy_entry("L", true, kNa, kNa, 40.0);
        if (position > 0.0) strategy_exit("x", "L", bar.close + 6.0, bar.close - 9.0);
        if (i == 50) strategy_entry("S", false);
        if (position < 0.0) strategy_exit("xs", "S", bar.close - 6.0, bar.close + 9.0);
        if (i == 64 || i == 82) strategy_close_all();
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

constexpr Scenario kScenarios[] = {Scenario::Flip, Scenario::Brackets, Scenario::Chains,
                                   Scenario::Margin};

#ifdef PINEFORGE_P7_HARVEST
void emit_values(const char* name, const char* what, const std::vector<std::uint64_t>& values) {
    std::printf("constexpr std::uint64_t k%s_%s[] = {", name, what);
    for (std::size_t i = 0; i < values.size(); ++i)
        std::printf("%s%lluull,", i % 3 == 0 ? "\n    " : " ",
                    static_cast<unsigned long long>(values[i]));
    std::printf("\n};\n");
}

void emit(Scenario scenario, const Observed& observed) {
    const char* name = name_of(scenario);
    emit_values(name, "rows", observed.rows);
    emit_values(name, "reads", observed.reads);
    std::printf("constexpr std::uint64_t k%s_final = %lluull;\n", name,
                static_cast<unsigned long long>(observed.final_hash));
    std::printf("constexpr Trade k%s_trades[] = {", name);
    for (const auto& t : observed.trades) {
        std::printf("\n    {%lldLL, %lldLL, %.17g, %.17g, %.17g, %d},",
                    static_cast<long long>(t.entry_time), static_cast<long long>(t.exit_time),
                    t.entry_price, t.exit_price, t.qty, t.open_at_end);
    }
    std::printf("\n};\n");
}
#else
#include "test_adapter_lookup_index_witness_data.hpp"

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

#define P7_EXPECTED(S) {Scenario::S, k##S##_rows, std::size(k##S##_rows), k##S##_reads, \
    std::size(k##S##_reads), k##S##_final, k##S##_trades, std::size(k##S##_trades)}
constexpr Expected kExpected[] = {
    P7_EXPECTED(Flip), P7_EXPECTED(Brackets), P7_EXPECTED(Chains), P7_EXPECTED(Margin),
};
#undef P7_EXPECTED

// The first observation that moved, so a failing lookup names the point.
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
#ifdef PINEFORGE_P7_HARVEST
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
    // The pins are non-trivial: every scenario trades, and its last row is
    // its final scalar.
    for (const auto& want : kExpected) {
        CHECK(want.trades_len >= 4);
        CHECK(want.rows_len != 0 && want.rows[want.rows_len - 1] == want.final_hash);
    }
    if (failures == 0)
        std::printf("test_adapter_lookup_index_witness: %d checks, 0 failures\n", checks);
    return failures == 0 ? 0 : 1;
#endif
}
