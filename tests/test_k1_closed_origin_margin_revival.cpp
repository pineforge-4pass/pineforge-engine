// CR-150 L19G / F2, G1, G2: preserve the retained engine's closed-origin leg
// revival through a sibling, global binding, or exposure returning in one cycle.
// Compare public results with every placement row retained. Keep the path
// controls: a missing fill, suspension, cohort pin or margin call is a failure.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_placement_retention.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

struct PairHoldTag {
    using type = void (ps::PineExecutionAdapter::*)(const ps::SourceId&);
    friend type access(PairHoldTag);
};
template struct PrivateAccess<PairHoldTag,
    &ps::PineExecutionAdapter::hold_reversal_pair_brackets>;

struct CycleTag {
    using type = std::int64_t ps::PineExecutionAdapter::*;
    friend type access(CycleTag);
};
template struct PrivateAccess<CycleTag, &ps::PineExecutionAdapter::current_position_cycle_>;

int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                          \
} while (false)

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

struct Leg {
    bool present = false;
    bool dormant = false;
    bool restored = false;
    bool withdrawn = false;
    std::uint64_t origin = 0;
    double qty = 0.0;
    double stop = kNa;
    std::uint64_t predecessor = 0;
};

struct Outcome {
    std::vector<std::uint64_t> observations;
    std::vector<double> positions;
    std::vector<Leg> legs;
    std::vector<std::size_t> extra_legs;
    std::vector<double> exposure;
    std::vector<std::int64_t> cycles;
    std::vector<Trade> trades;
    std::string error;
    double open = 0.0;
    double equity = 0.0;
};

enum class Scenario { Sibling, Global, Returning };

template<Scenario scenario>
class Host final : public ps::PineStrategyHost {
public:
    explicit Host(bool recording) {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 2;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(true);
        set_broker_state_hash_recording(recording);
    }

    void on_source_bar(const Bar&) override {
        observe();
        const int i = pine_bar_index();
        if constexpr (scenario == Scenario::Global) {
            // Empty entry IDs are admitted source IDs. R first opens the
            // cycle; global X is armed while held, on a DIFFERENT signal bar
            // from the later empty-ID entry. Its Applied callback binds the
            // live explicit leg to that parent without a same-bar re-arm.
            if (i == 0) strategy_entry("R", false, kNa, kNa, 40.0);
            if (i == 1)
                strategy_exit("X", "", kNa, 105.0, kNa, kNa, kNa, 100.0, {}, 40.0);
            if (i == 2) strategy_entry("", false, kNa, kNa, 40.0);
            if (i == 3) {
                // Remove the older R lot at this real callback's 100 print,
                // before S is admitted, so global X later closes its parent.
                strategy_close("R", "Release anchor", 40.0, kNa, true);
                strategy_entry("S", false, kNa, kNa, 40.0);
            }
            if (i == 7) {
                strategy_entry("L", true);
                strategy_close("S", "Reverse to Long");
            }
            return;
        }
        if constexpr (scenario == Scenario::Returning) {
            if (i == 0) strategy_entry("L", false, kNa, kNa, 40.0);
            if (i == 1) {
                strategy_exit("X", "L", kNa, 105.0, kNa, kNa, kNa,
                              100.0, "original X", 40.0);
                strategy_entry("R", false, kNa, kNa, 40.0);
            }
            if (i == 4) {
                // Use the real pair-hold policy in its live callback, as in
                // the bound fixture. This prepares a dormant predecessor for
                // the same-stop re-issue; no table row or event is invented.
                (adapter_.*access(PairHoldTag{}))("L");
            }
            if (i == 5) {
                strategy_entry("L", false, kNa, kNa, 40.0);
                strategy_exit("X", "L", kNa, 105.0, kNa, kNa, kNa,
                              100.0, "same-stop reissue", 40.0);
            }
            if (i == 6) strategy_close("R");
            if (i == 7) {
                strategy_entry("B", true);
                strategy_close("L", "Reverse to Long");
            }
            return;
        }
        if (i == 0) strategy_entry("S", false, kNa, kNa, 40.0);
        if (i == 1) {
            // Bind X only to the first, already-open lot. The second S is
            // placed AFTER the exit call, and fills before X reaches 105.
            strategy_exit("X", "S", kNa, 105.0, kNa, kNa, kNa, 100.0, {}, 40.0);
            strategy_entry("S", false, kNa, kNa, 40.0);
        }
        if (i == 5) {
            // The 160 gap declines the frozen all-in long / close pair.
            // The surviving same-id short keeps the original cycle open.
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
    }

    void observe() {
        BrokerStateHashSink f;
        f.i(pine_bar_index());
        f.d(live_position_size());
        f.d(position_avg_price());
        f.d(current_equity());
        f.i(trade_count());
        const int pending = pending_order_count();
        f.i(pending);
        for (int i = 0; i < pending; ++i) {
            double stop = kNa, limit = kNa, trail = kNa;
            f.i(pending_order_effective_levels(i, &stop, &limit, &trail));
            f.d(stop); f.d(limit); f.d(trail);
        }
        out_.observations.push_back(f.h);
        out_.positions.push_back(live_position_size());
        const char* const from_entry = scenario == Scenario::Global ? ""
            : scenario == Scenario::Returning ? "L" : "S";
        out_.exposure.push_back(adapter_.source_unclosed_qty_for(from_entry));
        out_.cycles.push_back(adapter_.*access(CycleTag{}));
        Leg leg;
        std::size_t extra = 0;
        for (const auto& row : adapter_.*access(PlacementTag{})) {
            const auto& snapshot = row.second;
            if (snapshot.family != ps::PineOrderFamily::ExitStop
                || snapshot.source_id != "X" || snapshot.from_entry != from_entry
                || snapshot.legs.target().incarnation == 0) continue;
            if (original_target_ == 0) original_target_ = snapshot.legs.target().incarnation;
            if (snapshot.legs.target().incarnation != original_target_) {
                ++extra;
                if constexpr (scenario == Scenario::Returning) {
                    // A later same-stop issue must not supersede original X.
                    CHECK(snapshot.exit_levels.stop == 105.0);
                    CHECK(snapshot.projection_predecessor != original_target_);
                }
                continue;
            }
            CHECK(!leg.present);
            leg.present = true;
            leg.dormant = snapshot.legs.dormant();
            leg.restored = snapshot.restored_after_margin;
            leg.withdrawn = snapshot.legs.retired(exit_legs::Leg::Stop)
                && snapshot.legs.retired(exit_legs::Leg::Limit)
                && snapshot.legs.retired(exit_legs::Leg::Trail);
            leg.origin = snapshot.bracket_origin.incarnation;
            leg.qty = snapshot.requested_qty;
            leg.stop = snapshot.exit_levels.stop;
            leg.predecessor = snapshot.projection_predecessor;
        }
        out_.legs.push_back(leg);
        out_.extra_legs.push_back(extra);
    }

    Outcome finish() {
        out_.error = last_error();
        out_.open = live_position_size();
        out_.equity = current_equity();
        for (int i = 0; i < trade_count(); ++i) out_.trades.push_back(get_trade(i));
        return out_;
    }

private:
    Outcome out_;
    // Observation identity only; no script decision reads it.
    std::uint64_t original_target_ = 0;
};

struct RetentionSwitch {
    explicit RetentionSwitch(bool retain) : previous(ps::detail::retain_retired_rows()) {
        ps::detail::set_retain_retired_rows(retain);
    }
    ~RetentionSwitch() { ps::detail::set_retain_retired_rows(previous); }
    bool previous;
};

std::vector<Bar> tape() {
    std::vector<Bar> bars;
    const auto add = [&](double o, double h, double l, double c) {
        bars.push_back({o, h, l, c, 1.0,
            kStart + static_cast<std::int64_t>(bars.size()) * 86400000LL});
    };
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0); // both same-id lots are now open
    add(100.0, 105.2, 99.8, 105.0); // X closes the first lot only
    add(105.0, 105.2, 104.8, 105.0); // the same-id sibling still needs the old leg
    add(105.0, 105.2, 104.8, 105.0); // place the reversal pair
    add(160.0, 160.2, 159.8, 160.0); // decline, below the short's margin level
    add(160.0, 160.2, 159.8, 160.0); // inspect the suspended retained leg
    add(160.0, 175.0, 159.8, 175.0); // a partial margin call, stop 105 reached
    add(175.0, 175.0, 175.0, 175.0);
    add(175.0, 175.0, 175.0, 175.0);
    return bars;
}

Outcome run(bool retain, bool magnifier, bool recording) {
    RetentionSwitch switch_for_run(retain);
    Host<Scenario::Sibling> host(recording);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "",
             magnifier, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

std::uint64_t trade_digest(const std::vector<Trade>& trades) {
    BrokerStateHashSink f;
    f.u(trades.size());
    for (const auto& t : trades) {
        f.i(t.entry_time); f.i(t.exit_time); f.d(t.entry_price); f.d(t.exit_price);
        f.d(t.qty); f.d(t.pnl); f.d(t.pnl_pct); f.b(t.is_long);
        f.i(t.entry_bar_index); f.i(t.exit_bar_index); f.s(t.entry_id); f.s(t.exit_id);
        f.s(t.entry_comment); f.s(t.exit_comment); f.b(t.exit_from_bracket);
        f.d(t.max_runup); f.d(t.max_drawdown); f.d(t.commission);
        f.u(t.entry_incarnation); f.b(t.open_at_end);
        f.u(static_cast<std::uint64_t>(t.close_cause));
    }
    return f.h;
}

void closed_origin_revival_matches_retained_execution() {
    for (bool magnifier : {false, true}) for (bool recording : {false, true}) {
        // Distribution samples check margin at bar 9's first 175 print;
        // the chart path checks the high on bar 8 (retained-engine trace).
        const int expected_margin_bar = magnifier ? 9 : 8;
        const auto erased = run(false, magnifier, recording);
        const auto retained = run(true, magnifier, recording);
        CHECK(erased.error.empty());
        CHECK(retained.error.empty());
        CHECK(erased.positions.size() == 11);
        CHECK(retained.positions.size() == 11);
        if (erased.positions.size() != 11 || retained.positions.size() != 11) continue;
        CHECK(erased.positions[2] == -80.0);
        CHECK(retained.positions[2] == -80.0);
        CHECK(erased.positions[5] == -40.0);
        CHECK(retained.positions[5] == -40.0);
        // Retained trace, bar 5: S2's exposure still pins S1's original X.
        CHECK(erased.legs[5].present && !erased.legs[5].dormant);
        CHECK(retained.legs[5].present && !retained.legs[5].dormant);
        CHECK(!retained.legs[5].withdrawn);
        CHECK(retained.legs[5].origin != 0);
        CHECK(retained.legs[5].qty == 40.0);
        CHECK(retained.legs[7].present && retained.legs[7].dormant);
        CHECK(!retained.legs[7].withdrawn && !retained.legs[7].restored);
        CHECK(erased.legs[7].present && erased.legs[7].dormant);
        CHECK(!erased.legs[7].withdrawn && !erased.legs[7].restored);
        CHECK(erased.positions[7] == -40.0);
        CHECK(retained.positions[7] == -40.0);

        int margin_calls = 0;
        int through_x = 0;
        for (const auto& trade : erased.trades) {
            if (trade.exit_id == "__margin_call__") {
                ++margin_calls;
                CHECK(trade.entry_id == "S");
                CHECK(trade.exit_bar_index == expected_margin_bar);
                CHECK(trade.qty > 0.0 && trade.qty < 40.0);
            }
            if (trade.exit_id == "X") {
                ++through_x;
                CHECK(trade.entry_id == "S");
                if (through_x == 1) {
                    // Retained trace, bar 3: the original X closes S1, 40 at 105.
                    CHECK(trade.entry_incarnation == retained.legs[5].origin);
                    CHECK(trade.entry_bar_index == 1);
                    CHECK(trade.qty == 40.0);
                    CHECK(trade.exit_price == 105.0);
                    CHECK(trade.exit_bar_index == 3);
                } else {
                    // Retained trace, margin: after the 32/7 slice, revived X
                    // closes S2's remaining 248/7 at 175 on the declared path.
                    CHECK(trade.entry_incarnation != retained.legs[5].origin);
                    CHECK(trade.entry_bar_index == 2);
                    CHECK(trade.qty == 248.0 / 7.0);
                    CHECK(trade.exit_price == 175.0);
                    CHECK(trade.exit_bar_index == expected_margin_bar);
                }
            }
        }
        CHECK(margin_calls > 0);
        // Retained trace: the original X, one margin slice, then revived X.
        CHECK(through_x == 2);
        CHECK(erased.trades.size() == 3);
        // Retained trace, after revival: S2 is flat; realized capital is
        // 10000 - (40 * 5) - (40 * 75) = 6800, including both S2 slices.
        CHECK(erased.open == 0.0);
        CHECK(erased.equity == 6800.0);
        CHECK(erased.equity > 0.0 && erased.equity < 9800.0);

        // F2 is closed only if retaining the dead leg changes no public result.
        // Do not exempt the reference from this check or harvest a new value.
        CHECK(erased.observations == retained.observations);
        CHECK(trade_digest(erased.trades) == trade_digest(retained.trades));
        CHECK(erased.open == retained.open);
        CHECK(erased.equity == retained.equity);
        // The observer still rejects duplicate original targets. Count the
        // distinct Flatten lifecycle too: none before margin, one afterwards.
        for (std::size_t i = 0; i < retained.extra_legs.size(); ++i) {
            if (static_cast<int>(i) < expected_margin_bar) {
                CHECK(retained.extra_legs[i] == 0);
                CHECK(erased.extra_legs[i] == 0);
            } else if (static_cast<int>(i) > expected_margin_bar) {
                CHECK(retained.extra_legs[i] == 1);
                CHECK(erased.extra_legs[i] == 1);
            }
        }
        CHECK(erased.legs[10].restored && retained.legs[10].restored);
        std::printf("F2 magnifier=%d recording=%d: trades %zu/%zu, open %.17g/%.17g, "
                    "equity %.17g/%.17g, retained X restored=%d\n",
                    magnifier ? 1 : 0, recording ? 1 : 0,
                    erased.trades.size(), retained.trades.size(), erased.open, retained.open,
                    erased.equity, retained.equity, retained.legs[9].restored ? 1 : 0);
    }
}

std::vector<Bar> returning_tape() {
    std::vector<Bar> bars;
    const auto add = [&](double o, double h, double l, double c) {
        bars.push_back({o, h, l, c, 1.0,
            kStart + static_cast<std::int64_t>(bars.size()) * 86400000LL});
    };
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0); // L1 and R hold the side
    add(100.0, 105.2, 99.8, 105.0); // original X closes L1 at 105
    add(105.0, 105.2, 104.8, 105.0); // sweep with L exposure zero, R open
    add(100.0, 100.2, 99.8, 100.0); // re-enter L and re-issue the same stop
    add(100.0, 100.2, 99.8, 100.0); // L2 opens at 100; request R's close
    add(100.0, 100.2, 99.8, 100.0); // R closes at 100; place reversal pair
    add(160.0, 160.2, 159.8, 160.0); // decline the frozen all-in reversal
    add(160.0, 160.2, 159.8, 160.0); // original X remains suspended
    add(160.0, 175.0, 159.8, 175.0); // chart margin checkpoint
    add(175.0, 175.0, 175.0, 175.0); // distribution-sample margin checkpoint
    add(175.0, 175.0, 175.0, 175.0);
    return bars;
}

std::vector<Bar> global_tape() {
    std::vector<Bar> bars;
    const auto add = [&](double o, double h, double l, double c) {
        bars.push_back({o, h, l, c, 1.0,
            kStart + static_cast<std::int64_t>(bars.size()) * 86400000LL});
    };
    add(100.0, 100.2, 99.8, 100.0); // submit R
    add(100.0, 100.2, 99.8, 100.0); // R fills at 100; arm origin-free global X
    add(100.0, 100.2, 99.8, 100.0); // submit the empty-ID parent on a later bar
    add(100.0, 100.2, 99.8, 100.0); // parent fills at 100, binds X; close R, submit S
    add(100.0, 100.2, 99.8, 100.0); // S fills at 100; parent and S hold 80 units
    add(100.0, 105.2, 99.8, 105.0); // global X closes its 40-unit parent at 105
    add(105.0, 105.2, 104.8, 105.0); // sweep with empty-ID exposure zero, S open
    add(105.0, 105.2, 104.8, 105.0); // place the all-in reversal / close pair
    add(160.0, 160.2, 159.8, 160.0); // decline the reversal; suspend global X
    add(160.0, 160.2, 159.8, 160.0); // inspect the original dormant global leg
    add(160.0, 175.0, 159.8, 175.0); // chart margin checkpoint revives X
    add(175.0, 175.0, 175.0, 175.0); // sampled path's first 175 checkpoint
    add(175.0, 175.0, 175.0, 175.0);
    return bars;
}

template<Scenario scenario>
Outcome run_cycle_case(bool retain, bool magnifier, bool recording) {
    RetentionSwitch switch_for_run(retain);
    Host<scenario> host(recording);
    const auto bars = scenario == Scenario::Returning ? returning_tape() : global_tape();
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "",
             magnifier, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

template<Scenario scenario>
void cycle_revival_matches_retained_execution() {
    constexpr bool returning = scenario == Scenario::Returning;
    constexpr std::size_t count = 13;
    constexpr std::size_t two_open = returning ? 2 : 4;
    constexpr std::size_t closed_origin = returning ? 4 : 6;
    const char* const entry = returning ? "L" : "S";
    for (bool magnifier : {false, true}) for (bool recording : {false, true}) {
        // Both retained traces reach 175 on bar 10; distribution samples
        // arm margin only at the first 175 print of bar 11.
        const int expected_margin_bar = 10 + (magnifier ? 1 : 0);
        // Expected trading controls below describe the RETAINED path. The
        // erasing run is compared with it, never used to derive an expectation.
        const auto retained = run_cycle_case<scenario>(true, magnifier, recording);
        const auto erased = run_cycle_case<scenario>(false, magnifier, recording);
        CHECK(retained.error.empty());
        CHECK(erased.error.empty());
        CHECK(retained.positions.size() == count);
        CHECK(erased.positions.size() == count);
        if (retained.positions.size() != count || erased.positions.size() != count) continue;
        // G2: its empty-ID parent entered on bar 3, S on bar 4, and X closed
        // only the parent on bar 5. R was already closed at 100 on bar 3.
        // G1 retains L1/R's existing bar-2 and closed-origin bar-4 controls.
        CHECK(retained.positions[two_open] == -80.0);
        CHECK(retained.positions[closed_origin] == -40.0);
        CHECK(retained.legs[closed_origin].present && !retained.legs[closed_origin].dormant);
        CHECK(erased.legs[closed_origin].present && !erased.legs[closed_origin].dormant);
        CHECK(retained.legs[closed_origin].origin != 0);
        CHECK(retained.legs[closed_origin].qty == 40.0
            && retained.legs[closed_origin].stop == 105.0);
        CHECK(retained.legs[closed_origin].predecessor == 0);
        constexpr std::size_t suspended = 9;
        CHECK(retained.positions[suspended] == -40.0);
        CHECK(erased.positions[suspended] == -40.0);
        CHECK(retained.legs[suspended].present && retained.legs[suspended].dormant);
        CHECK(erased.legs[suspended].present && erased.legs[suspended].dormant);
        CHECK(!retained.legs[suspended].withdrawn && !retained.legs[suspended].restored);
        CHECK(!erased.legs[suspended].withdrawn && !erased.legs[suspended].restored);
        if constexpr (returning) {
            CHECK(retained.exposure[4] == 0.0 && erased.exposure[4] == 0.0);
            CHECK(retained.exposure[6] == 40.0 && erased.exposure[6] == 40.0);
            CHECK(retained.cycles[2] == retained.cycles[4]);
            CHECK(retained.cycles[4] == retained.cycles[6]);
            CHECK(erased.cycles[2] == erased.cycles[6]);
            // One same-stop successor coexists, without consuming old X.
            CHECK(retained.extra_legs[7] == 1 && erased.extra_legs[7] == 1);
            CHECK(retained.legs[7].dormant && erased.legs[7].dormant);
        } else {
            // X starts global and origin-free under R; the empty-ID opening
            // binds it on bar 3. Its cohort later has no units, while S's
            // different ID still holds 40 and keeps this position cycle open.
            CHECK(retained.legs[2].present && retained.legs[2].origin == 0);
            CHECK(erased.legs[2].present && erased.legs[2].origin == 0);
            CHECK(retained.exposure[4] == 40.0 && erased.exposure[4] == 40.0);
            CHECK(retained.exposure[6] == 0.0 && erased.exposure[6] == 0.0);
            CHECK(retained.legs[4].present && erased.legs[4].present);
            CHECK(retained.legs[4].origin == retained.legs[6].origin);
            CHECK(erased.legs[4].origin == erased.legs[6].origin);
            CHECK(retained.cycles[2] == retained.cycles[6]);
            CHECK(erased.cycles[2] == erased.cycles[6]);
            std::printf("G2 retained trace magnifier=%d recording=%d: bound origin %llu/%llu, "
                        "after-close present=%d dormant=%d, after-decline dormant=%d, "
                        "original restored=%d\n",
                        magnifier ? 1 : 0, recording ? 1 : 0,
                        static_cast<unsigned long long>(retained.legs[4].origin),
                        static_cast<unsigned long long>(retained.legs[6].origin),
                        retained.legs[6].present ? 1 : 0, retained.legs[6].dormant ? 1 : 0,
                        retained.legs[9].dormant ? 1 : 0, retained.legs.back().restored ? 1 : 0);
        }

        int through_x = 0;
        int margin_calls = 0;
        int r_closes = 0;
        for (const auto& trade : retained.trades) {
            if (trade.exit_id == "__margin_call__") {
                ++margin_calls;
                CHECK(trade.entry_id == entry);
                CHECK(trade.entry_bar_index == (returning ? 6 : 4));
                CHECK(trade.exit_bar_index == expected_margin_bar);
                // Retained path at 175: equity 9800 - 40*75 = 6800,
                // maintenance 7000; the 4x shortfall slice is 4*200/175.
                CHECK(trade.exit_price == 175.0);
                CHECK(trade.qty == 32.0 / 7.0);
            } else if (trade.exit_id == "X") {
                ++through_x;
                CHECK(trade.entry_price == 100.0);
                if constexpr (returning) CHECK(trade.exit_comment == "original X");
                if (through_x == 1) {
                    // Retained G2: the empty-ID parent opens at 100 on bar 3;
                    // global X closes exactly that parent, 40 at 105 on bar 5.
                    CHECK(trade.entry_id == (returning ? "L" : ""));
                    CHECK(trade.entry_incarnation == retained.legs[closed_origin].origin);
                    CHECK(trade.entry_bar_index == (returning ? 1 : 3));
                    CHECK(trade.exit_bar_index == (returning ? 3 : 5));
                    CHECK(trade.exit_price == 105.0);
                    CHECK(trade.qty == 40.0);
                } else {
                    CHECK(trade.entry_incarnation != retained.legs[closed_origin].origin);
                    CHECK(trade.entry_id == entry);
                    // G2's S opens at 100 on bar 4. At 175 the 32/7 margin
                    // slice leaves 248/7 for the original global leg to close.
                    CHECK(trade.entry_bar_index == (returning ? 6 : 4));
                    CHECK(trade.exit_bar_index == expected_margin_bar);
                    CHECK(trade.exit_price == 175.0);
                    CHECK(trade.qty == 248.0 / 7.0);
                }
            } else {
                ++r_closes;
                CHECK(trade.entry_id == "R");
                // Retained G2's anchor is closed immediately at bar 3's
                // 100 print; G1 keeps its original bar-2/bar-7 R round trip.
                CHECK(trade.entry_bar_index == (returning ? 2 : 1));
                CHECK(trade.exit_bar_index == (returning ? 7 : 3));
                CHECK(trade.qty == 40.0);
                CHECK(trade.entry_price == 100.0 && trade.exit_price == 100.0);
                CHECK(trade.pnl == 0.0);
            }
        }
        CHECK(through_x == 2);
        CHECK(margin_calls == 1);
        CHECK(r_closes == 1);
        // Each retained trace has R's zero-PnL close, original X, one margin
        // slice and revived X, with no fifth trade.
        CHECK(retained.trades.size() == 4);
        CHECK(retained.legs.back().restored && erased.legs.back().restored);
        CHECK(retained.open == 0.0);
        // Retained path: the original X loses 40*5 = 200; the remaining
        // lot loses 40*75 = 3000. Each case's R round trip contributes zero.
        CHECK(retained.equity == 6800.0);
        CHECK(erased.observations == retained.observations);
        CHECK(trade_digest(erased.trades) == trade_digest(retained.trades));
        CHECK(erased.open == retained.open);
        CHECK(erased.equity == retained.equity);
        std::printf("%s magnifier=%d recording=%d: trades %zu/%zu, open %.17g/%.17g, "
                    "equity %.17g/%.17g, original restored=%d/%d\n",
                    returning ? "G1 returning exposure" : "G2 global parent leg",
                    magnifier ? 1 : 0, recording ? 1 : 0,
                    erased.trades.size(), retained.trades.size(), erased.open, retained.open,
                    erased.equity, retained.equity,
                    erased.legs.back().restored ? 1 : 0, retained.legs.back().restored ? 1 : 0);
    }
}
} // namespace

int main() {
    closed_origin_revival_matches_retained_execution();
    cycle_revival_matches_retained_execution<Scenario::Global>();
    cycle_revival_matches_retained_execution<Scenario::Returning>();
    std::printf("test_k1_closed_origin_margin_revival: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
