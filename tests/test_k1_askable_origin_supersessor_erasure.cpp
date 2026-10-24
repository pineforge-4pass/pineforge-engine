// R6a-F1 reproduction: compare the same public-order tape with retirement
// erasing rows and retaining every row. No placement/lifecycle is fabricated.
// Harness: test_k1_closed_origin_margin_revival.cpp / test_k1_origin_pin_bound.cpp.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_placement_retention.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace ps = pineforge::source;

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
struct PlacementTag {
    using type = ps::PlacementTable ps::PineExecutionAdapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag, &ps::PineExecutionAdapter::placement_>;
struct CycleTag {
    using type = std::int64_t ps::PineExecutionAdapter::*;
    friend type access(CycleTag);
};
template struct PrivateAccess<CycleTag, &ps::PineExecutionAdapter::current_position_cycle_>;

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
constexpr std::int64_t kDay = 86400000LL;
constexpr std::array<int, 4> kIssueBars{{1, 2, 4, 6}};
constexpr std::array<const char*, 4> kNames{{"Y", "A", "B", "C"}};

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "64-bit double required");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

std::vector<Bar> tape() {
    std::vector<Bar> bars;
    const auto add = [&](double o, double h, double l, double c) {
        bars.push_back({o, h, l, c, 1.0,
            kStart + static_cast<std::int64_t>(bars.size()) * kDay});
    };
    add(100.0, 100.2, 99.8, 100.0); // b0: submit S, explicit 40
    add(100.0, 100.2, 99.8, 100.0); // b1: S fills; Y, dynamic stop 150
    add(100.0, 100.2, 99.8, 100.0); // b2: A, stop 120 / 20; first reversal pair
    add(115.0, 121.0, 114.0, 115.0); // b3: gap declines L; A touched and refused
    add(115.0, 116.0, 114.0, 115.0); // b4: B, stop 125 / 20; second pair
    add(123.0, 126.0, 114.0, 115.0); // b5: gap declines L; B touched and refused
    add(115.0, 116.0, 114.0, 115.0); // b6: C, stop 120 / 20, no reversal pair
    add(115.0, 116.0, 114.0, 115.0); // b7: sweep; none of the stops reached
    add(115.0, 300.0, 114.0, 300.0); // b8: C at 120, then margin at the high
    return bars;
}

struct Row {
    bool present = false;
    bool live = false; // native working book, independently of dormancy
    bool dormant = false;
    bool withdrawn = false;
    bool restored = false;
    bool stop_available = false;
    bool opening = false;
    bool reverse_to = false;
    bool signal_refused = false;
    int family = -1;
    int created_bar = -1;
    std::uint64_t incarnation = 0;
    std::uint64_t target = 0;
    std::uint64_t origin = 0;
    std::uint64_t predecessor = 0;
    std::int64_t cycle = 0;
    std::int64_t script_ms = 0;
    std::int64_t hold_bar = -1;
    std::int64_t excluded_bar = -1;
    double qty = kNa;
    double percent = kNa;
    double stop = kNa;
    double frozen_units = kNa;
    double sizing_equity = kNa;
    double sizing_price = kNa;
    std::string source_id;
    std::string from_entry;
    std::string comment;
};

struct Observation {
    int bar = -1;
    bool after_commands = false;
    Bar prices{};
    double position = 0.0;
    double average = kNa;
    double equity_api = 0.0;
    double exposure = 0.0;
    std::int64_t cycle = 0;
    int trades = 0;
    int pending = 0;
    std::array<Row, 4> legs{};
    std::array<int, 4> matches{};
    std::vector<Row> rows;
    std::vector<std::uint64_t> working;
};

struct Outcome {
    bool retain_seen = false;
    std::vector<Observation> observations;
    std::vector<Trade> trades;
    std::string error;
    double position = 0.0;
    double equity = 0.0;
};

class Host final : public ps::PineStrategyHost {
public:
    explicit Host(bool recording) {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(true);
        set_broker_state_hash_recording(recording);
    }

    void on_source_bar(const Bar& bar) override {
        observe(bar, false);
        const int i = pine_bar_index();
        if (i == 0) strategy_entry("S", false, kNa, kNa, 40.0);
        if (i == 1) {
            // Omitting qty keeps Y origin-free, independent of A/B/C's key.
            strategy_exit("X", "S", kNa, 150.0, kNa, kNa, kNa, 100.0, "Y");
        }
        if (i == 2 || i == 4) {
            strategy_exit("X", "S", kNa, i == 2 ? 120.0 : 125.0,
                          kNa, kNa, kNa, 100.0, i == 2 ? "A" : "B", 20.0);
            // The real close lowering calls the pair hold. Explicit exits
            // drain after this callback; the declined reversal suspends
            // those fresh legs at the next open.
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
        if (i == 6) {
            strategy_exit("X", "S", kNa, 120.0, kNa, kNa, kNa, 100.0, "C", 20.0);
        }
        observe(bar, true);
    }

    Outcome finish() {
        out_.retain_seen = ps::detail::retain_retired_rows();
        out_.error = last_error();
        out_.position = live_position_size();
        out_.equity = current_equity();
        for (int i = 0; i < trade_count(); ++i) out_.trades.push_back(get_trade(i));
        return out_;
    }

private:
    void observe(const Bar& bar, bool after_commands) {
        Observation observation;
        observation.bar = pine_bar_index();
        observation.after_commands = after_commands;
        observation.prices = bar;
        observation.position = live_position_size();
        observation.average = position_avg_price();
        observation.equity_api = current_equity();
        observation.exposure = adapter_.source_unclosed_qty_for("S");
        observation.cycle = adapter_.*access(CycleTag{});
        observation.trades = trade_count();
        observation.pending = pending_order_count();
        for (const auto& working : native_working_requests())
            observation.working.push_back(working.definition->handle.incarnation);
        const auto& placements = adapter_.*access(PlacementTag{});
        for (const auto& placement : placements) {
            const auto& snapshot = placement.second;
            Row row;
            row.present = true;
            row.incarnation = placement.first;
            for (const auto incarnation : observation.working)
                if (incarnation == row.incarnation) row.live = true;
            row.family = static_cast<int>(snapshot.family);
            row.created_bar = snapshot.projection_created_bar;
            row.source_id = snapshot.source_id;
            row.from_entry = snapshot.from_entry;
            row.comment = snapshot.comment;
            row.target = snapshot.legs.target().incarnation;
            row.origin = snapshot.bracket_origin.incarnation;
            row.predecessor = snapshot.projection_predecessor;
            row.cycle = snapshot.placement_cycle;
            row.script_ms = snapshot.placement_script_open_ms;
            row.qty = snapshot.requested_qty;
            row.percent = snapshot.qty_percent;
            row.stop = snapshot.exit_levels.stop;
            row.dormant = snapshot.legs.dormant();
            row.withdrawn = snapshot.legs.retired(exit_legs::Leg::Stop)
                && snapshot.legs.retired(exit_legs::Leg::Limit)
                && snapshot.legs.retired(exit_legs::Leg::Trail);
            row.restored = snapshot.restored_after_margin;
            row.stop_available = snapshot.legs.available(
                exit_legs::Leg::Stop, observation.bar);
            row.hold_bar = snapshot.legs.hold_bar();
            row.excluded_bar = snapshot.legs.excluded_bar();
            row.opening = snapshot.opening;
            row.reverse_to = snapshot.reverse_to;
            row.signal_refused = snapshot.signal_price_refused;
            row.frozen_units = snapshot.sizing.frozen_units;
            row.sizing_equity = snapshot.sizing.equity;
            row.sizing_price = snapshot.sizing.price;
            observation.rows.push_back(row);
            if (snapshot.family != ps::PineOrderFamily::ExitStop
                || row.source_id != "X" || row.from_entry != "S" || row.target == 0)
                continue;
            for (std::size_t slot = 0; slot < kIssueBars.size(); ++slot) {
                if (row.script_ms != kStart + kIssueBars[slot] * kDay) continue;
                ++observation.matches[slot];
                observation.legs[slot] = row;
            }
        }
        out_.observations.push_back(observation);
    }

    // Output only: neither order decisions nor adapter hashing reads this.
    Outcome out_;
};

struct RetentionSwitch {
    explicit RetentionSwitch(bool retain) : previous(ps::detail::retain_retired_rows()) {
        ps::detail::set_retain_retired_rows(retain);
    }
    ~RetentionSwitch() { ps::detail::set_retain_retired_rows(previous); }
    bool previous;
};

Outcome run(bool retain) {
    RetentionSwitch switch_for_run(retain);
    Host host(true);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "",
             false, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

void print_trade(const char* mode, std::size_t index, const Trade& trade) {
    std::printf(
        "%s trade[%zu] entry_id=\"%s\" exit_id=\"%s\" entry_bar=%d exit_bar=%d "
        "entry_time=%lld exit_time=%lld qty=%.17g entry_price=%.17g exit_price=%.17g "
        "pnl=%.17g pnl_pct=%.17g long=%d entry_comment=\"%s\" exit_comment=\"%s\" "
        "bracket=%d runup=%.17g drawdown=%.17g commission=%.17g "
        "entry_incarnation=%llu open_at_end=%d close_cause=%u\n",
        mode, index, trade.entry_id.c_str(), trade.exit_id.c_str(),
        trade.entry_bar_index, trade.exit_bar_index,
        static_cast<long long>(trade.entry_time), static_cast<long long>(trade.exit_time),
        trade.qty, trade.entry_price, trade.exit_price, trade.pnl, trade.pnl_pct,
        trade.is_long ? 1 : 0, trade.entry_comment.c_str(), trade.exit_comment.c_str(),
        trade.exit_from_bracket ? 1 : 0, trade.max_runup, trade.max_drawdown,
        trade.commission, static_cast<unsigned long long>(trade.entry_incarnation),
        trade.open_at_end ? 1 : 0, static_cast<unsigned>(trade.close_cause));
}

void print_outcome(const char* mode, const Outcome& outcome) {
    std::printf("%s retain_switch=%d error=\"%s\" final_position=%.17g equity_api=%.17g\n",
                mode, outcome.retain_seen ? 1 : 0, outcome.error.c_str(),
                outcome.position, outcome.equity);
    for (const auto& observation : outcome.observations) {
        std::printf(
            "%s b%d %s OHLC=%.17g/%.17g/%.17g/%.17g position=%.17g avg=%.17g "
            "equity_api=%.17g S_exposure=%.17g cycle=%lld trades=%d pending=%d working=",
            mode, observation.bar, observation.after_commands ? "after" : "before",
            observation.prices.open, observation.prices.high,
            observation.prices.low, observation.prices.close, observation.position,
            observation.average, observation.equity_api, observation.exposure,
            static_cast<long long>(observation.cycle),
            observation.trades, observation.pending);
        for (const auto incarnation : observation.working)
            std::printf("%llu,", static_cast<unsigned long long>(incarnation));
        std::printf("\n");
        for (std::size_t slot = 0; slot < kNames.size(); ++slot) {
            const auto& row = observation.legs[slot];
            std::printf(
                "  %s present=%d matches=%d inc=%llu live=%d dormant=%d withdrawn=%d "
                "qty=%.17g stop=%.17g stop_bits=%016llx origin=%llu pred=%llu "
                "restored=%d stop_available=%d hold_bar=%lld excluded_bar=%lld cycle=%lld\n",
                kNames[slot], row.present ? 1 : 0, observation.matches[slot],
                static_cast<unsigned long long>(row.incarnation), row.live ? 1 : 0,
                row.dormant ? 1 : 0, row.withdrawn ? 1 : 0, row.qty, row.stop,
                static_cast<unsigned long long>(bits(row.stop)),
                static_cast<unsigned long long>(row.origin),
                static_cast<unsigned long long>(row.predecessor),
                row.restored ? 1 : 0, row.stop_available ? 1 : 0,
                static_cast<long long>(row.hold_bar), static_cast<long long>(row.excluded_bar),
                static_cast<long long>(row.cycle));
        }
        // Include every actual row so missing, replaced or deferred named
        // legs and the reversal's source sizing remain diagnosable.
        for (const auto& row : observation.rows) {
            std::printf(
                "  row inc=%llu family=%d id=\"%s\" from=\"%s\" comment=\"%s\" "
                "created_bar=%d script_ms=%lld target=%llu origin=%llu pred=%llu "
                "live=%d dormant=%d withdrawn=%d qty=%.17g percent=%.17g stop=%.17g "
                "opening=%d reverse_to=%d signal_refused=%d frozen_units=%.17g "
                "sizing_equity=%.17g sizing_price=%.17g\n",
                static_cast<unsigned long long>(row.incarnation), row.family,
                row.source_id.c_str(), row.from_entry.c_str(), row.comment.c_str(),
                row.created_bar, static_cast<long long>(row.script_ms),
                static_cast<unsigned long long>(row.target),
                static_cast<unsigned long long>(row.origin),
                static_cast<unsigned long long>(row.predecessor),
                row.live ? 1 : 0, row.dormant ? 1 : 0, row.withdrawn ? 1 : 0,
                row.qty, row.percent, row.stop, row.opening ? 1 : 0,
                row.reverse_to ? 1 : 0, row.signal_refused ? 1 : 0,
                row.frozen_units, row.sizing_equity, row.sizing_price);
        }
    }
    std::printf("%s trades=%zu\n", mode, outcome.trades.size());
    for (std::size_t i = 0; i < outcome.trades.size(); ++i)
        print_trade(mode, i, outcome.trades[i]);
}

const Observation* after(const Outcome& outcome, int bar) {
    for (const auto& observation : outcome.observations)
        if (observation.bar == bar && observation.after_commands) return &observation;
    return nullptr;
}

const Observation* before(const Outcome& outcome, int bar) {
    for (const auto& observation : outcome.observations)
        if (observation.bar == bar && !observation.after_commands) return &observation;
    return nullptr;
}

bool control(const char* mode, const char* name, bool holds) {
    std::printf("K1-CONTROL %s %s: %s\n", mode, name, holds ? "PASS" : "FAIL");
    if (!holds) std::printf("K1-REPRO: CONTROL FAILED [%s] %s\n", mode, name);
    return holds;
}

bool frozen_pair_was_declined(const Observation& settled, int signal_bar) {
    for (const auto& row : settled.rows) {
        if (row.family == static_cast<int>(ps::PineOrderFamily::Entry)
            && row.source_id == "L" && row.opening && row.reverse_to && !row.live
            && !row.signal_refused && std::isnan(row.qty)
            && row.script_ms == kStart + signal_bar * kDay
            && std::isfinite(row.frozen_units) && row.frozen_units > 0.0
            && std::isfinite(row.sizing_equity)
            && row.frozen_units * settled.prices.open
                > row.sizing_equity + std::abs(row.sizing_equity) * 1e-12)
            return true;
    }
    return false;
}

bool controls(const char* mode, const Outcome& outcome, bool retain) {
    if (!control(mode, "retention mode", outcome.retain_seen == retain)) return false;
    if (!control(mode, "run completed without error", outcome.error.empty())) return false;
    if (!control(mode, "nine before/after callbacks",
                 outcome.observations.size() == 18)) return false;
    for (int bar = 0; bar <= 8; ++bar)
        if (!control(mode, "bar has before/after command observations",
                     before(outcome, bar) != nullptr
                     && after(outcome, bar) != nullptr)) return false;
    const auto& b1 = *after(outcome, 1);
    const auto& b3 = *after(outcome, 3);
    const auto& b5 = *after(outcome, 5);
    // A new explicit leg drains only AFTER on_source_bar returns. Read C
    // on the next callback, after b7's sweep and before any new commands.
    const auto& b7_before = *before(outcome, 7);
    const auto& b7 = *after(outcome, 7);
    const auto& y = b1.legs[0];
    const auto& a = b3.legs[1];
    const auto& b = b5.legs[2];
    const auto& c = b7_before.legs[3];
    if (!control(mode, "b1 short 40 at 100",
                 b1.position == -40.0 && b1.average == 100.0)) return false;
    if (!control(mode, "b1 Y live, origin zero, no qty, stop 150",
                 b1.matches[0] == 1 && y.present && y.live && !y.dormant
                 && !y.withdrawn && y.origin == 0 && std::isnan(y.qty)
                 && y.stop == 150.0)) return false;
    if (!control(mode, "b3 first public reversal declined at gap open",
                 frozen_pair_was_declined(b3, 2) && b3.position == -40.0))
        return false;
    if (!control(mode, "b3 A present, dead/dormant, origin open, touched 120",
                 b3.position == -40.0 && b3.exposure == 40.0
                 && b3.matches[1] == 1 && a.present && !a.live && a.dormant
                 && !a.withdrawn && a.origin != 0 && a.qty == 20.0 && a.stop == 120.0
                 && a.predecessor == 0
                 && b3.prices.open < a.stop && b3.prices.high >= a.stop)) return false;
    if (!control(mode, "b5 second public reversal declined at gap open",
                 frozen_pair_was_declined(b5, 4) && b5.position == -40.0))
        return false;
    if (!control(mode, "b5 B present, dead/dormant, same origin, touched 125",
                 b5.position == -40.0 && b5.exposure == 40.0
                 && b5.matches[2] == 1 && b.present && !b.live && b.dormant
                 && !b.withdrawn && b.origin == a.origin && b.qty == 20.0
                 && b.stop == 125.0 && b.predecessor == 0
                 && b5.prices.open < b.stop && b5.prices.high >= b.stop)) return false;
    if (!control(mode, "b6 C live at b7-before, same origin, A=C stop bits != B",
                 b7_before.matches[3] == 1 && c.present && c.live && !c.dormant
                 && !c.withdrawn && c.qty == 20.0 && c.origin == a.origin
                 && c.predecessor == 0 && bits(c.stop) == bits(a.stop)
                 && bits(c.stop) != bits(b.stop)
                 && a.incarnation != b.incarnation && b.incarnation != c.incarnation
                 && a.cycle == b.cycle && b.cycle == c.cycle)) return false;
    if (!control(mode, "b7 after sweep: Y root live, A dormant, C live, short 40",
                 b7.position == -40.0 && b7.legs[0].present && b7.legs[0].live
                 && b7.legs[0].origin == 0 && b7.legs[0].dormant
                 && b7.legs[1].present && b7.legs[1].dormant && !b7.legs[1].live
                 && b7.legs[3].present && b7.legs[3].live && !b7.legs[3].dormant
                 && b7.prices.high < c.stop)) return false;
    int c_fills = 0;
    bool margin_on_b8 = false;
    bool premature_close = false;
    for (const auto& trade : outcome.trades) {
        if (!trade.open_at_end && trade.exit_bar_index < 8) premature_close = true;
        if (trade.exit_id == "X" && trade.exit_comment == "C"
            && trade.entry_id == "S" && trade.entry_bar_index == 1
            && trade.exit_bar_index == 8 && !trade.open_at_end
            && trade.qty == 20.0 && trade.exit_price == 120.0)
            ++c_fills;
        if (trade.exit_id == "__margin_call__" && trade.exit_bar_index == 8
            && trade.qty > 0.0 && !trade.open_at_end)
            margin_on_b8 = true;
    }
    if (!control(mode, "no close before b8", !premature_close)) return false;
    if (!control(mode, "b8 C fills 20 at 120", c_fills == 1)) return false;
    if (!control(mode, "b8 margin call fires", margin_on_b8)) return false;
    return true;
}

bool same_number(double lhs, double rhs) {
    return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
}

const char* first_differing_field(const Trade& lhs, const Trade& rhs) {
#define K1_FIELD(field) if (lhs.field != rhs.field) return #field
#define K1_NUMBER(field) if (!same_number(lhs.field, rhs.field)) return #field
    K1_FIELD(entry_time);
    K1_FIELD(exit_time);
    K1_NUMBER(entry_price);
    K1_NUMBER(exit_price);
    K1_NUMBER(qty);
    K1_NUMBER(pnl);
    K1_NUMBER(pnl_pct);
    K1_FIELD(is_long);
    K1_FIELD(entry_bar_index);
    K1_FIELD(exit_bar_index);
    K1_FIELD(entry_id);
    K1_FIELD(entry_comment);
    K1_FIELD(exit_comment);
    K1_FIELD(exit_id);
    K1_FIELD(exit_from_bracket);
    K1_NUMBER(max_runup);
    K1_NUMBER(max_drawdown);
    K1_NUMBER(commission);
    K1_FIELD(entry_incarnation);
    K1_FIELD(open_at_end);
    K1_FIELD(close_cause);
#undef K1_NUMBER
#undef K1_FIELD
    return nullptr;
}

int reproduce() {
    const Outcome erased = run(false);
    const Outcome retained = run(true);
    // Always print all observations and both complete trade lists, including
    // on a setup failure. No literal trade roster is an oracle.
    print_outcome("ERASE", erased);
    print_outcome("RETAIN", retained);
    if (!controls("ERASE", erased, false) || !controls("RETAIN", retained, true))
        return 1;
    std::size_t first = 0;
    while (first < erased.trades.size() && first < retained.trades.size()) {
        if (const char* field = first_differing_field(
                erased.trades[first], retained.trades[first])) {
            std::printf("K1-REPRO: RED erase!=retain first_trade=%zu field=%s\n",
                        first, field);
            print_trade("ERASE first difference", first, erased.trades[first]);
            print_trade("RETAIN first difference", first, retained.trades[first]);
            return 1;
        }
        ++first;
    }
    if (erased.trades.size() != retained.trades.size()) {
        std::printf(
            "K1-REPRO: RED erase!=retain first_trade=%zu field=trade_count "
            "ERASE=%zu RETAIN=%zu\n", first, erased.trades.size(), retained.trades.size());
        if (first < erased.trades.size())
            print_trade("ERASE first difference", first, erased.trades[first]);
        else
            std::printf("ERASE first difference trade[%zu] <missing>\n", first);
        if (first < retained.trades.size())
            print_trade("RETAIN first difference", first, retained.trades[first]);
        else
            std::printf("RETAIN first difference trade[%zu] <missing>\n", first);
        return 1;
    }
    std::printf("K1-REPRO: GREEN (erase == retain)\n");
    return 0;
}
} // namespace

int main() {
    try {
        return reproduce();
    } catch (const std::exception& error) {
        std::printf("K1-REPRO: CONTROL FAILED [exception] %s\n", error.what());
        return 1;
    }
}
