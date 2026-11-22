/*
 * test_exit_bracket_pending_entry_leg.cpp — TV binds exit brackets to ENTRY
 * INSTANCES via from_entry, not to the net position.
 *
 * A strategy.exit re-issue MODIFIES every live leg carrying that exit id (each
 * keeping its own binding) and ADDITIONALLY arms one new leg bound to the
 * pending entry order whose id == from_entry, if one exists at that moment.
 * The engine armed exactly ONE order per exit id, so a re-issue that coincided
 * with a pending same-id entry under-reserved: two qty=1 brackets covered only
 * 2 units of a position that became 4.
 *
 * The paired-bracket probe's rule, illustrated synthetically,
 * pyramiding=2, ETH-USDT-USDT 15m. The three shapes below must
 * hold SIMULTANEOUSLY; each one falsifies a different naive rule.
 *
 * (i)  LEG MULTIPLICITY. A 2u long carried from a prior bar with both
 *      LongT1/LongT2 live; an OR bar re-issues both brackets (stop
 *      re-priced to orLow=1410.00) while a same-id 2u entry is pending.
 *      The stop closes FOUR units at 1410.00 later, tagged T1/T2/T1/T2 —
 *      a full bracket pair PER ENTRY INSTANCE, carried pair first (FIFO):
 *          T1 1410.00 q1  carried entry @1400.00
 *          T2 1410.00 q1  carried entry @1400.00
 *          T1 1410.00 q1  added entry   @1420.00
 *          T2 1410.00 q1  added entry   @1420.00
 *      Engine pre-fix: only the carried pair closed; the added 2u collapsed
 *      into one unprotected trade that survived to a later reversal.
 *
 * (ii) THE COUNTER-CASE that refutes a blanket multiply. Same
 *      shape (live carried Short bracket + pending same-id entry, both
 *      brackets re-issued in the same block) but ShortT1 had already been
 *      CONSUMED earlier, so it has no live leg and arms only the ONE
 *      pending-entry leg. A single T1 later closes exactly 1 unit
 *      (the carried remnant, FIFO). `reserved = qty * (open legs + pending
 *      entries)` would close 2 units at 1422.00 and desync later trades —
 *      a regression on a locus the engine already matches. Green both pre- and
 *      post-fix by construction: it is the guard, not the repro.
 *
 * (iii) NO ADMISSIBLE PENDING ENTRY. The third short is over
 *      pyramiding=2 and TV rejects it, so the re-issue binds to nothing new:
 *      ShortT1 (no live leg, no admissible pending entry) arms NOTHING because
 *      the two live ShortT2 legs already reserve the whole position, and
 *      ShortT2 re-arms BOTH its legs. A later stop @1426.00 closes 2 units
 *      tagged "T2 Exit" / "T2 Exit" — the double-T2 label that only this model
 *      reproduces. Needs the pyramiding-count fix (an entry the cap will
 *      refuse at fill contributes no bracket leg). Assertions retain original inputs.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;
static int setup_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

// Failed foundations are errors, never evidence for a reservation mismatch.
#define CHECK_SETUP(expr)                                                       \
    do {                                                                       \
        const bool setup_ok = static_cast<bool>(expr);                          \
        CHECK(setup_ok);                                                       \
        if (!setup_ok) {                                                       \
            std::printf("SETUP FAILURE %s:%d %s\n", __FILE__, __LINE__, #expr); \
            ++setup_failed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

namespace {

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(double o, double h, double l, double c, int64_t ts) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

class BracketProbe : public pineforge::source::PineStrategyHost {
public:
    BracketProbe() {
        initial_capital_ = 1000000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        pyramiding_ = 2;
        process_orders_on_close_ = false;
    }

    std::string exit_id(int i) const { return closed_trade_exit_id(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double size(int i) const { return closed_trade_size(i); }
    double position_size() const { return signed_position_size(); }

    int rows_with_exit_id(const std::string& xid) const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (closed_trade_exit_id(i) == xid) ++n;
        }
        return n;
    }

protected:
    // The strategy's bracket pair: qty=1 T1/T2 legs sharing one stop, both
    // attached to entry id "L" — the paired-bracket probe's shape.
    void arm_brackets(double t1_limit, double t2_limit, double stop) {
        strategy_exit("T1", "L", t1_limit, stop, kNaN, kNaN, kNaN,
                      100.0, "", 1.0);
        strategy_exit("T2", "L", t2_limit, stop, kNaN, kNaN, kNaN,
                      100.0, "", 1.0);
    }
};

// ── (i) A re-issue over a pending same-id entry arms both legs ─────────────
//
//  bar 0  entry L(2u) + T1/T2 (stop 90)          [flat: one deferred leg each]
//  bar 1  L fills @100                            pos 2u  [L:2]
//  bar 2  entry L(2u) AGAIN + T1/T2 re-issued (stop 95) while L is pending
//         -> T1: 1 live leg + 1 pending entry = 2 legs
//         -> T2: 1 live leg + 1 pending entry = 2 legs
//  bar 3  L#2 fills @110                          pos 4u  [L:2 @100, L:2 @110]
//  bar 4  low 90 crosses the 95 stop -> ALL FOUR legs fire @95
class CarriedPairProbe : public BracketProbe {
public:
    void on_source_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(200.0, 300.0, 90.0);
                break;
            case 2:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(210.0, 310.0, 95.0);
                break;
            default: break;
        }
    }
};

static std::vector<Bar> carried_pair_bars() {
    return {
        mk(100, 101,  99, 100, 1000),   // 0
        mk(100, 101,  99, 100, 2000),   // 1  L fills @100
        mk(100, 101,  99, 100, 3000),   // 2  re-issue over the pending L
        mk(110, 111, 105, 110, 4000),   // 3  L#2 fills @110
        mk(105, 106,  90,  95, 5000),   // 4  stop 95 crossed
        mk( 95,  96,  94,  95, 6000),   // 5
    };
}

// ── (ii)+(iii): consumed leg, then a cap-refused entry ─────────────────────
//
//  bar 0  entry L(2u) + T1(lim 110)/T2(lim 150), stop 80
//  bar 1  L fills @100                            pos 2u  [A:2]
//  bar 2  high 111 -> T1 limit 110 fills 1u       pos 1u  [A:1]   T1 CONSUMED
//  bar 3  entry L(2u) + T1(lim 105)/T2(lim 150) re-issued while L is pending
//         -> T1: 0 live legs + 1 pending entry = ONE leg      <- (ii)
//         -> T2: 1 live leg  + 1 pending entry = two legs
//  bar 4  L#2 fills @100                          pos 3u  [A:1, B:2]  count 2
//  bar 5  high 106 -> T1 limit 105 fills exactly 1u (FIFO -> A's remnant)
//         pos 2u [B:2]; entry count stays 2 (monotone)
//         then: entry L(2u) + T1(lim 200)/T2(lim 250) re-issued, stop 95
//         -> the pending L is over pyramiding=2: no admissible pending entry
//         -> T1: 0 live legs, and the two live T2 legs already reserve the
//                whole 2u position -> NOTHING armed                <- (iii)
//         -> T2: 2 live legs, no pending entry -> BOTH re-armed
//  bar 6  L#3's fill attempt is refused by the pyramiding cap  pos 2u
//  bar 7  low 90 crosses the 95 stop -> 2 units @95, BOTH tagged T2
class ConsumedLegProbe : public BracketProbe {
public:
    void on_source_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(110.0, 150.0, 80.0);
                break;
            case 3:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(105.0, 150.0, 80.0);
                break;
            case 5:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(200.0, 250.0, 95.0);
                break;
            default: break;
        }
    }
};

static std::vector<Bar> consumed_leg_bars() {
    return {
        mk(100, 101,  99, 100, 1000),   // 0
        mk(100, 101,  99, 100, 2000),   // 1  L fills @100
        mk(100, 111,  99, 100, 3000),   // 2  T1 limit 110 -> 1u
        mk(100, 101,  99, 100, 4000),   // 3  re-issue over the pending L
        mk(100, 101,  99, 100, 5000),   // 4  L#2 fills @100
        mk(100, 106,  99, 100, 6000),   // 5  T1 limit 105 -> exactly 1u
        mk(100, 101,  99, 100, 7000),   // 6  L#3 refused by the cap
        mk(100, 101,  90,  95, 8000),   // 7  stop 95 crossed
        mk( 95,  96,  94,  95, 9000),   // 8
    };
}

}  // namespace

// ---- (i) four units exit at the stop, a full pair per entry instance -------

static void test_reissue_over_pending_entry_arms_a_pair_per_instance() {
    std::printf("test_reissue_over_pending_entry_arms_a_pair_per_instance\n");
    CarriedPairProbe eng;
    auto bars = carried_pair_bars();
    eng.run(bars.data(), (int)bars.size());

    // TV's row ordering: T1/T2 against the carried lot (FIFO first), then T1/T2
    // against the added lot. Pre-fix only the first two rows existed and 2
    // units survived unprotected.
    CHECK(eng.trade_count() == 4);
    CHECK(eng.rows_with_exit_id("T1") == 2);
    CHECK(eng.rows_with_exit_id("T2") == 2);
    for (int i = 0; i < eng.trade_count() && i < 4; ++i) {
        CHECK(near(eng.size(i), 1.0));
        CHECK(near(eng.exit_price(i), 95.0));
    }
    if (eng.trade_count() == 4) {
        CHECK(eng.exit_id(0) == std::string("T1"));
        CHECK(near(eng.entry_price(0), 100.0));   // carried lot
        CHECK(eng.exit_id(1) == std::string("T2"));
        CHECK(near(eng.entry_price(1), 100.0));   // carried lot
        CHECK(eng.exit_id(2) == std::string("T1"));
        CHECK(near(eng.entry_price(2), 110.0));   // added lot
        CHECK(eng.exit_id(3) == std::string("T2"));
        CHECK(near(eng.entry_price(3), 110.0));   // added lot
    }
    CHECK(near(eng.position_size(), 0.0));        // pre-fix: 2.0 survived
}

// ---- (ii) a consumed leg arms ONE leg; the limit closes exactly 1 unit -----
// ---- (iii) a cap-refused entry arms no new leg; both T2 legs re-price ------

static void test_consumed_leg_and_capped_entry() {
    std::printf("test_consumed_leg_and_capped_entry\n");
    ConsumedLegProbe eng;
    auto bars = consumed_leg_bars();
    eng.run(bars.data(), (int)bars.size());

    // rows: T1@110 (1u), T1@105 (1u), then the 2-unit stop-out @95.
    CHECK(eng.trade_count() == 4);
    if (eng.trade_count() < 4) return;

    // (ii) the 2026-03-27 locus. ShortT1 had no live leg, so the re-issue arms
    // exactly ONE leg and the limit touch closes exactly ONE unit — FIFO
    // against the carried remnant. A blanket qty*(legs+pending) multiply would
    // close 2 here.
    CHECK(eng.exit_id(0) == std::string("T1"));
    CHECK(near(eng.size(0), 1.0));
    CHECK(near(eng.exit_price(0), 110.0));
    CHECK(eng.exit_id(1) == std::string("T1"));
    CHECK(near(eng.size(1), 1.0));
    CHECK(near(eng.exit_price(1), 105.0));
    CHECK(near(eng.entry_price(1), 100.0));       // the carried remnant, FIFO

    // (iii) the 2026-03-29 locus. The over-cap entry never fills, so no third
    // lot appears; T1 arms nothing and BOTH surviving T2 legs fire at the stop.
    CHECK(eng.exit_id(2) == std::string("T2"));
    CHECK(near(eng.size(2), 1.0));
    CHECK(near(eng.exit_price(2), 95.0));
    CHECK(eng.exit_id(3) == std::string("T2"));
    CHECK(near(eng.size(3), 1.0));
    CHECK(near(eng.exit_price(3), 95.0));
    CHECK(eng.rows_with_exit_id("T1") == 2);      // never a third T1
    CHECK(eng.rows_with_exit_id("T2") == 2);
    CHECK(near(eng.position_size(), 0.0));
}

// ---- rerun determinism (handle reuse) --------------------------------------

static void test_rerun_reproduces_the_leg_census() {
    std::printf("test_rerun_reproduces_the_leg_census\n");
    CarriedPairProbe eng;
    auto bars = carried_pair_bars();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 4);

    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 4);
    CHECK(eng.rows_with_exit_id("T1") == 2);
    CHECK(eng.rows_with_exit_id("T2") == 2);
    CHECK(near(eng.position_size(), 0.0));
}

namespace {

class PartialReservationProbe : public source::PineStrategyHost {
public:
    struct Opening {
        std::uint64_t origin;
        std::string id;
        int bar;
        double units;
    };
    PartialReservationProbe(const source::PineStrategyConfig& config,
                            double tick, double quantity_step) {
        attach_pine_execution_adapter();
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(tick);
        set_syminfo_metadata("qty_step", quantity_step);
    }

    using source::PineStrategyHost::native_state;
    double position() const { return signed_position_size(); }
    std::vector<Opening> openings;

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                const NativeDecisionContext& context) override {
        if (std::abs(event.opened_units) > 0.0) {
            openings.push_back({event.handle().incarnation, event.request().label,
                               static_cast<int>(context.coordinate.interval_index),
                               std::abs(event.opened_units)});
        }
    }
};

static bool run_reservation(PartialReservationProbe& probe,
                            const std::vector<Bar>& bars, const std::string& label) {
    probe.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK_SETUP(probe.last_error().empty());
    if (!probe.last_error().empty()) {
        std::printf("SETUP FAILURE [%s]: %s\n", label.c_str(), probe.last_error().c_str());
        return false;
    }
    return true;
}

#include "exit_retention_cases.hpp"

struct FillPlacements {
    using Type = source::PlacementTable source::PineExecutionAdapter::*;
    friend Type retention_member(FillPlacements);
};
template struct RetentionMember<FillPlacements, &source::PineExecutionAdapter::placement_>;

class AddBarPercentProbe final : public PartialReservationProbe {
public:
    explicit AddBarPercentProbe(int add_bar, bool from_average = false, int close_delay = 1)
        : PartialReservationProbe(config(), 0.01, 0.00001), add_bar_(add_bar),
          from_average_(from_average), close_delay_(close_delay) {}

    struct TakeProfit { std::uint64_t handle, origin; int bar; double units, reserved; };
    std::vector<TakeProfit> fills;

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 || bar_index_ == add_bar_ - 1)
            strategy_entry("Long", true, kNaN, kNaN, 0.02);
        if (position() > 0.0)
            strategy_exit("TP", "Long", from_average_ ? position_avg_price() * 1.01 : 101,
                          kNaN, kNaN, kNaN, kNaN, 50.0);
        if (bar_index_ == add_bar_ + close_delay_) strategy_close("Long");
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        PartialReservationProbe::on_native_applied_event(event, context);
        if (event.request().label != "TP") return;
        const auto handle = event.handle().incarnation;
        const auto& placements = adapter_.*retention_member(FillPlacements{});
        const auto leg = placements.find(handle);
        CHECK(leg != placements.end());
        if (leg != placements.end())
            fills.push_back({handle, leg->second.per_entry_origin,
                static_cast<int>(context.coordinate.interval_index), event.closed_units,
                leg->second.per_entry_units});
    }

private:
    static source::PineStrategyConfig config() {
        source::PineStrategyConfig cfg;
        cfg.initial_capital = 10000;
        cfg.pyramiding = 2;
        cfg.close_entries_rule_any = false;
        return cfg;
    }
    int add_bar_;
    bool from_average_;
    int close_delay_;
};

static void test_percent_exit_on_add_fill_bar() {
    for (bool gap : {false, true}) {
        const int add_bar = gap ? 2 : 3;
        AddBarPercentProbe probe(add_bar);
        std::vector<Bar> bars;
        for (int i = 0; i <= add_bar + 2; ++i) {
            const auto time = (i + 1) * 60000LL;
            if (i < add_bar) bars.push_back(mk(100, 100.25, 99.75, 100, time));
            else if (i == add_bar && !gap) bars.push_back(mk(100.5, 102, 100.25, 101.5, time));
            else bars.push_back(mk(102, 103, 101.5, 102, time));
        }
        if (!run_reservation(probe, bars, "percent exit on add fill")) continue;
        CHECK_SETUP(probe.openings.size() == 2);
        if (probe.openings.size() != 2) continue;
        const auto& first = probe.openings[0];
        const auto& add = probe.openings[1];
        CHECK_SETUP(first.bar == 1 && add.bar == add_bar && first.origin != add.origin);
        CHECK_SETUP(near(first.units, 0.02) && near(add.units, 0.02));

        // Each fill spends its own opening's share; FIFO books both against the first lot.
        CHECK(probe.fills.size() == 2);
        if (probe.fills.size() == 2) {
            CHECK(probe.fills[0].handle != probe.fills[1].handle);
            for (const auto& opening : probe.openings)
                CHECK(std::count_if(probe.fills.begin(), probe.fills.end(), [&](const auto& fill) {
                    return fill.origin == opening.origin && fill.bar == add_bar
                        && near(fill.units, 0.01) && near(fill.reserved, 0.01);
                }) == 1);
        }
        CHECK(probe.trade_count() == 3);
        if (probe.trade_count() != 3) continue;
        for (int i = 0; i != 2; ++i) {
            const auto trade = probe.get_trade(i);
            CHECK(trade.exit_id == "TP" && trade.exit_bar_index == add_bar);
            CHECK(trade.entry_incarnation == first.origin && near(trade.qty, 0.01));
            CHECK(near(trade.exit_price, gap ? 102.0 : 101.0));
        }
        const auto remainder = probe.get_trade(2);
        CHECK(remainder.exit_id == "__close__Long" && remainder.exit_bar_index == add_bar + 2);
        CHECK(remainder.entry_incarnation == add.origin && near(remainder.qty, 0.02));
    }
}

static void test_percent_add_after_first_take_profit() {
    // The first opening has already spent TP when the add is placed. Its
    // remaining 0.01 lot still sets the limit and is first in the FIFO book.
    for (bool resting : {false, true}) {
        AddBarPercentProbe probe(3, true, 2);
        const std::vector<Bar> bars{
            mk(100, 100, 100, 100, 60000),
            mk(100, 100, 100, 100, 120000),
            mk(101, 102, 100, 100, 180000),  // first TP at 101; queue the add
            resting ? mk(98.5, 100, 98, 99, 240000) : mk(102, 103, 101, 102, 240000),
            mk(99, 101, 98, 100, 300000),   // repriced TP at 99.99 if still working
            mk(101, 103, 100, 102, 360000), // another touch must not spend either share again
            mk(102, 103, 101, 102, 420000),
        };
        if (!run_reservation(probe, bars, "add after spent percentage exit")) continue;
        CHECK_SETUP(probe.openings.size() == 2);
        if (probe.openings.size() != 2) continue;
        const auto& first = probe.openings[0];
        const auto& add = probe.openings[1];
        CHECK_SETUP(first.bar == 1 && add.bar == 3 && first.origin != add.origin);
        CHECK_SETUP(near(first.units, 0.02) && near(add.units, 0.02));
        CHECK(probe.fills.size() == 2);
        if (probe.fills.size() == 2) {
            CHECK(probe.fills[0].bar == 2 && probe.fills[0].origin == first.origin);
            CHECK(probe.fills[1].bar == (resting ? 4 : 3) && probe.fills[1].origin == add.origin);
            CHECK(probe.fills[0].handle != probe.fills[1].handle);
            for (const auto& fill : probe.fills)
                CHECK(near(fill.units, 0.01) && near(fill.reserved, 0.01));
        }
        CHECK(probe.trade_count() == 3);
        if (probe.trade_count() != 3) continue;
        const auto tp = probe.get_trade(1);
        CHECK(tp.exit_id == "TP" && tp.entry_incarnation == first.origin && near(tp.qty, 0.01));
        CHECK(tp.exit_bar_index == (resting ? 4 : 3) && near(tp.exit_price, resting ? 99.99 : 102));
        const auto tail = probe.get_trade(2);
        CHECK(tail.exit_id == "__close__Long" && tail.exit_bar_index == 6);
        CHECK(tail.entry_incarnation == add.origin && near(tail.qty, 0.02));
    }
}

class PercentAfterFillProbe final : public PartialReservationProbe {
public:
    PercentAfterFillProbe() : PartialReservationProbe(configuration(), 0.01, 1.0) {}

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, 4.0);
        if (bar_index_ == 3) strategy_entry("L", true, kNaN, kNaN, 6.0);
        if (bar_index_ >= 1 && bar_index_ <= 6)
            strategy_exit("TP", "L", 110.0, kNaN, kNaN, kNaN, kNaN, 50.0);
        if (bar_index_ == 5) working = adapter_.fixture_pending_snapshots();
        if (bar_index_ == 7) strategy_close("L");
    }

    struct Fill {
        std::uint64_t handle;
        int bar;
        double units;
        std::size_t first_row, rows;
    };
    std::vector<Fill> tp;
    std::vector<source::PineExecutionAdapter::FixturePendingSnapshot> working;

private:
    static source::PineStrategyConfig configuration() {
        source::PineStrategyConfig cfg;
        cfg.initial_capital = 100000.0;
        cfg.pyramiding = 2;
        cfg.close_entries_rule_any = false;
        cfg.margin_long = cfg.margin_short = 0.0;
        return cfg;
    }

    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        PartialReservationProbe::on_native_applied_event(event, context);
        if (event.request().label == "TP" && event.closed_units > 0.0)
            tp.push_back({event.handle().incarnation, event.interval_index(), event.closed_units,
                          event.first_trade_index, event.closed_trade_count});
    }
};

static void test_percentage_reissue_after_fill_covers_only_the_add() {
    PercentAfterFillProbe probe;
    const std::vector<Bar> bars = {
        mk(100, 101, 99, 100,  60000),  // submit 4
        mk(100, 101, 99, 100, 120000),  // open 4, place TP
        mk(100, 111, 99, 100, 180000),  // TP spends the first opening's 2
        mk(100, 111, 99, 100, 240000),  // retouch, then submit 6
        mk(104, 105, 103, 104, 300000), // open 6; reissue with both lots held
        mk(104, 105, 103, 104, 360000), // reissue again before the touch
        mk(104, 111, 103, 104, 420000), // the add owns 3; FIFO books 2 + 1
        mk(104, 111, 103, 104, 480000), // retouch; close the remainder
        mk(103, 104, 102, 103, 540000),
    };
    if (!run_reservation(probe, bars, "percent-after-fill")) return;
    CHECK_SETUP(probe.openings.size() == 2);
    if (probe.openings.size() != 2) return;
    const auto& first = probe.openings[0];
    const auto& add = probe.openings[1];
    CHECK_SETUP(first.id == "L" && first.bar == 1 && near(first.units, 4.0));
    CHECK_SETUP(add.id == "L" && add.bar == 4 && near(add.units, 6.0));
    CHECK_SETUP(first.origin != 0 && add.origin != 0 && first.origin != add.origin);
    CHECK_SETUP(!probe.tp.empty() && probe.tp[0].bar == 2 && near(probe.tp[0].units, 2.0));
    double add_bar_tp_units = 0;
    for (const auto& fill : probe.tp)
        if (fill.bar == 6) add_bar_tp_units += fill.units;
    CHECK(near(add_bar_tp_units, 3.0)); // Half the 6-unit add.
    CHECK(probe.tp.size() == 2);
    if (probe.tp.size() != 2) return;
    const auto& fill = probe.tp[1];
    CHECK(fill.handle != probe.tp[0].handle && fill.bar == 6 && near(fill.units, 3.0));
    CHECK(fill.first_row == 1 && fill.rows == 2);
    const auto leg = std::find_if(probe.working.begin(), probe.working.end(),
        [&](const auto& row) { return !row.staged && row.incarnation == fill.handle; });
    CHECK(leg != probe.working.end());
    if (leg != probe.working.end()) {
        CHECK(leg->snapshot.source_id == "TP" && leg->snapshot.from_entry == "L");
        CHECK(leg->snapshot.bracket_origin.incarnation == add.origin);
        CHECK(leg->snapshot.per_entry_origin == add.origin && near(leg->snapshot.per_entry_units, 3.0));
        CHECK(std::isnan(leg->snapshot.requested_qty));
        CHECK(near(leg->snapshot.qty_percent, 50.0));
        CHECK(near(leg->snapshot.projection_remaining_qty, 3.0));
    }
    CHECK(probe.trade_count() == 4);
    if (probe.trade_count() != 4) return;
    for (int i = 0; i < 4; ++i) {
        const auto row = probe.get_trade(i);
        CHECK(!row.open_at_end && row.is_long);
        CHECK(row.entry_id == "L" && row.entry_incarnation == (i < 2 ? first.origin : add.origin));
        CHECK(row.entry_bar_index == (i < 2 ? 1 : 4));
        CHECK(near(row.entry_price, i < 2 ? 100.0 : 104.0));
        CHECK(row.exit_id == (i < 3 ? "TP" : "__close__L"));
        CHECK(row.exit_bar_index == (i == 0 ? 2 : i < 3 ? 6 : 8));
        CHECK(near(row.exit_price, i < 3 ? 110.0 : 103.0));
        CHECK(near(row.qty, i < 2 ? 2.0 : i == 2 ? 1.0 : 5.0));
    }
    CHECK(near(probe.position(), 0.0));
}

enum class FillSequence { Drained, Intervening, SameBar, Gap };

struct SubmitPlacement {
    using Type = std::optional<native_order::RequestHandle> (source::PineExecutionAdapter::*)(
        native_order::Request, source::PlacementSnapshot, bool, const source::SourceId&);
    friend Type retention_member(SubmitPlacement);
};
template struct RetentionMember<SubmitPlacement, &source::PineExecutionAdapter::submit_or_replace>;

class FillSequenceProbe final : public PartialReservationProbe {
public:
    explicit FillSequenceProbe(FillSequence sequence)
        : PartialReservationProbe(configuration(), 0.01, sequence == FillSequence::Drained ? 1.0 : 0.0),
          sequence_(sequence) {}

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) entry(4);
        if (bar_index_ == 1) {
            if (sequence_ == FillSequence::Drained) take();
            else unbound_take();
            if (sequence_ == FillSequence::SameBar)
                strategy_entry("L", true, 95, kNaN, 6);
        }
        if (bar_index_ == 2) {
            if (sequence_ != FillSequence::SameBar) entry(6);
            if (sequence_ == FillSequence::Gap || sequence_ == FillSequence::SameBar) take();
        }
        if (bar_index_ == 3 && (sequence_ == FillSequence::Drained || sequence_ == FillSequence::Intervening))
            strategy_order("Other", false, sequence_ == FillSequence::Drained ? 2 : 1);
        if (bar_index_ == 4 && (sequence_ == FillSequence::Drained || sequence_ == FillSequence::Intervening))
            take();
        if (bar_index_ == 6) strategy_close("L");
    }

    struct Fill { std::string id; int bar; double opened, closed, price; std::uint64_t owner, bound; };
    std::vector<Fill> fills;

private:
    static source::PineStrategyConfig configuration() {
        source::PineStrategyConfig cfg;
        cfg.pyramiding = 2;
        return cfg;
    }
    void entry(double qty) { strategy_entry("L", true, kNaN, kNaN, qty); }
    void take() { strategy_exit("TP", "L", 110, kNaN, kNaN, kNaN, kNaN, 50); }
    void unbound_take() {
        // Start with a named physical book close with no per-opening binding.
        // The engine sizes and matches it, and the real Applied callback must
        // date it even though no earlier per-opening request was installed.
        native_order::Request request;
        request.intent = native_order::HostSized{native_order::HostSizedKind::Close, std::nullopt};
        request.label = "TP";
        request.trigger = native_order::Limit{110};
        source::PlacementSnapshot leg;
        leg.family = source::PineOrderFamily::ExitLimit;
        leg.source_id = "TP";
        leg.from_entry = "L";
        leg.qty_percent = 50;
        leg.deferred_cohort = leg.fixed_exit_reservation = true;
        leg.exit_levels.limit = 110;
        const auto handle = (adapter_.*retention_member(SubmitPlacement{}))(
            std::move(request), std::move(leg), false, "unbound TP");
        CHECK_SETUP(handle.has_value());
    }
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        PartialReservationProbe::on_native_applied_event(event, context);
        const auto& placements = adapter_.*retention_member(FillPlacements{});
        const auto row = placements.find(event.handle().incarnation);
        fills.push_back({event.request().label, event.interval_index(), event.opened_units,
            event.closed_units, event.resolved_price, row == placements.end() ? 0 : row->second.per_entry_origin,
            row == placements.end() ? 0 : row->second.bracket_origin.incarnation});
    }
    FillSequence sequence_;
};

static void test_spent_tp_fill_order() {
    for (const auto sequence : {FillSequence::Drained, FillSequence::Intervening,
                                FillSequence::SameBar, FillSequence::Gap}) {
        const bool same_bar = sequence == FillSequence::SameBar;
        const bool gap = sequence == FillSequence::Gap;
        const bool other = !same_bar && !gap;
        FillSequenceProbe probe(sequence);
        const std::vector<Bar> bars{
            mk(100, 101, 99, 100, 60000), mk(100, 101, 99, 100, 120000),
            mk(110, 112, same_bar ? 94 : 105, 108, 180000),
            gap ? mk(112, 114, 111, 113, 240000) :
                same_bar ? mk(105, 111, 104, 105, 240000) : mk(105, 106, 104, 105, 240000),
            mk(105, 106, 104, 105, 300000), mk(105, 111, 104, 105, 360000),
            mk(105, 111, 104, 105, 420000), mk(103, 104, 102, 103, 480000),
        };
        if (!run_reservation(probe, bars, "spent TP fill order")) continue;
        CHECK_SETUP(probe.openings.size() == 2);
        if (probe.openings.size() != 2) continue;
        CHECK_SETUP(probe.openings[0].bar == 1 && near(probe.openings[0].units, 4));
        CHECK_SETUP(probe.openings[1].bar == (same_bar ? 2 : 3) && near(probe.openings[1].units, 6));
        CHECK_SETUP(probe.fills.size() >= 3);
        if (probe.fills.size() < 3) continue;
        const auto& first_tp = probe.fills[1];
        CHECK_SETUP(first_tp.id == "TP" && first_tp.bar == 2
                    && near(first_tp.closed, 2) && near(first_tp.price, 110));
        if (sequence != FillSequence::Drained)
            CHECK_SETUP(first_tp.owner == 0 && first_tp.bound == 0);
        const auto& add_fill = probe.fills[2];
        CHECK_SETUP(add_fill.id == "L" && near(add_fill.opened, 6));
        if (other) {
            CHECK_SETUP(probe.fills.size() > 3 && probe.fills[3].id == "Other"
                && probe.fills[3].bar == 4 && near(probe.fills[3].closed, sequence == FillSequence::Drained ? 2 : 1));
        }
        const int touch = other ? 5 : 3;
        CHECK(std::count_if(probe.fills.begin(), probe.fills.end(), [&](const auto& f) {
            return f.id == "TP" && f.bar == touch && near(f.closed, 3)
                && near(f.price, gap ? 112 : 110) && f.owner == probe.openings[1].origin;
        }) == 1);
        // In all cases B owns three. The unrelated close determines how
        // many of those three come from A's last two FIFO units.
        const int older = sequence == FillSequence::Drained ? 0 : sequence == FillSequence::Intervening ? 1 : 2;
        int rows = 0;
        for (int i = 0; i < probe.trade_count(); ++i) {
            const auto trade = probe.get_trade(i);
            if (trade.exit_id != "TP" || trade.exit_bar_index == 2) continue;
            const bool from_first = older != 0 && rows == 0;
            CHECK(trade.exit_bar_index == touch);
            CHECK(trade.entry_incarnation == probe.openings[from_first ? 0 : 1].origin);
            CHECK(near(trade.qty, from_first ? older : 3 - older));
            ++rows;
        }
        CHECK(rows == (older == 0 ? 1 : 2));
        CHECK(near(probe.position(), 0));
    }
}

class PendingAddsProbe final : public PartialReservationProbe {
public:
    explicit PendingAddsProbe(bool spent)
        : PartialReservationProbe(configuration(), 0.01, 1.0), spent_(spent) {}
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, 4);
        if (bar_index_ == 1)
            strategy_exit("TP", "L", spent_ ? 110 : 140, kNaN, kNaN, kNaN, kNaN, 50);
        if (bar_index_ == 2) {
            // Distinct native placements, one Pine id. A source-level
            // replacement of the first handle would not exercise this case.
            for (int qty : {6, 8}) {
                native_order::Request request;
                request.label = "L";
                request.intent = native_order::Transact{static_cast<double>(qty)};
                source::PlacementSnapshot parent;
                parent.family = source::PineOrderFamily::Entry;
                parent.source_id = "L";
                parent.is_long = true;
                parent.requested_qty = qty;
                const auto handle = (adapter_.*retention_member(SubmitPlacement{}))(
                    std::move(request), std::move(parent), true, qty == 6 ? "first add" : "second add");
                CHECK_SETUP(handle.has_value());
            }
            strategy_exit("TP", "L", 110, kNaN, kNaN, kNaN, kNaN, 50);
        }
        if (bar_index_ == 4) strategy_close("L");
    }
    struct Share { int bar; double units; std::uint64_t owner; };
    std::vector<Share> shares;
private:
    bool spent_;
    static source::PineStrategyConfig configuration() {
        source::PineStrategyConfig cfg;
        cfg.pyramiding = 3;
        cfg.margin_long = cfg.margin_short = 0;
        return cfg;
    }
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        PartialReservationProbe::on_native_applied_event(event, context);
        if (event.request().label != "TP") return;
        const auto& rows = adapter_.*retention_member(FillPlacements{});
        const auto leg = rows.find(event.handle().incarnation);
        shares.push_back({event.interval_index(), event.closed_units,
                          leg == rows.end() ? 0 : leg->second.per_entry_origin});
    }
};

static void test_each_pending_add_owns_a_share() {
    for (bool spent : {false, true}) {
        PendingAddsProbe probe(spent);
        std::vector<Bar> bars;
        for (int i = 0; i < 6; ++i)
            bars.push_back(mk(100, i >= 2 ? 111 : 101, 99, 100, (i + 1) * 60000LL));
        if (!run_reservation(probe, bars, "each pending add")) continue;
        CHECK_SETUP(probe.openings.size() == 3);
        if (probe.openings.size() != 3) continue;
        const double quantities[] = {4, 6, 8};
        for (int i = 0; i < 3; ++i) {
            CHECK_SETUP(probe.openings[i].bar == (i == 0 ? 1 : 3));
            CHECK_SETUP(near(probe.openings[i].units, quantities[i]));
            CHECK(std::count_if(probe.shares.begin(), probe.shares.end(), [&](const auto& share) {
                return share.owner == probe.openings[i].origin && near(share.units, quantities[i] / 2)
                    && share.bar == (i == 0 && spent ? 2 : 3);
            }) == 1);
        }
        CHECK(probe.shares.size() == 3 && near(probe.position(), 0));
    }
}

// Stage copies of a live limit in the three queues that can hold its
// reissue until a later execution phase.
struct BracketQueue { friend auto queue_member(BracketQueue); };
struct RecalcQueue { friend auto queue_member(RecalcQueue); };
struct DelayQueue { friend auto queue_member(DelayQueue); };
template<class Tag, auto Member> struct QueueMember {
    friend auto queue_member(Tag) { return Member; }
};
template struct QueueMember<BracketQueue, &source::PineExecutionAdapter::pending_bracket_legs_>;
template struct QueueMember<RecalcQueue, &source::PineExecutionAdapter::pending_coof_requests_>;
template struct QueueMember<DelayQueue, &source::PineExecutionAdapter::delayed_market_orders_>;

class ZeroShareProbe final : public PartialReservationProbe {
public:
    ZeroShareProbe() : PartialReservationProbe(configuration(), 0.01, 1.0) {}

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 1)
            strategy_entry("L", true, kNaN, kNaN, bar_index_ == 0 ? 4 : 2);
        if (bar_index_ == 2) {
            percentage("ahead", 140, 50);
            percentage("TP", 110, 50);
        }
        if (bar_index_ == 3) {
            CHECK_SETUP(openings.size() == 2);
            if (openings.size() != 2) return;
            // The earlier exit reserves 3 + 1 of six units. The two left
            // for TP all belong to its first opening; the second gets zero.
            percentage("ahead", 140, 75);
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                if (row.staged || row.snapshot.source_id != "TP"
                    || row.snapshot.per_entry_origin != openings[1].origin) continue;
                const auto key = std::string("TP\x1fL")
                    + std::to_string(static_cast<int>(source::PineOrderFamily::ExitLimit))
                    + "\x1f" + std::to_string(openings[1].origin);
                native_order::Request request;
                request.label = "TP";
                request.intent = native_order::HostSized{native_order::HostSizedKind::Close, std::nullopt};
                request.trigger = native_order::Limit{110};
                auto& brackets = adapter_.*queue_member(BracketQueue{});
                auto& recalc = adapter_.*queue_member(RecalcQueue{});
                auto& delayed = adapter_.*queue_member(DelayQueue{});
                brackets.push_back({request, row.snapshot, key, 0});
                recalc.push_back({request, row.snapshot, key, false, 0, true});
                delayed.push_back({request, row.snapshot, key, 0});
                staged = true;
            }
            percentage("TP", 110, 50);
            const auto own = [&](const auto& row) {
                return row.snapshot.source_id == "TP" && row.snapshot.per_entry_origin == openings[1].origin;
            };
            const auto clear = [&](const auto& queue) {
                return std::none_of(queue.begin(), queue.end(), own);
            };
            CHECK(clear(adapter_.*queue_member(BracketQueue{})));
            CHECK(clear(adapter_.*queue_member(RecalcQueue{})));
            CHECK(clear(adapter_.*queue_member(DelayQueue{})));
        }
        if (bar_index_ == 5) strategy_close("L");
    }
    bool staged = false;
private:
    static source::PineStrategyConfig configuration() {
        source::PineStrategyConfig cfg;
        cfg.pyramiding = 2;
        return cfg;
    }
    void percentage(const char* id, double level, double percent) {
        strategy_exit(id, "L", level, kNaN, kNaN, kNaN, kNaN, percent);
    }
};

static void test_zero_share_cannot_fill() {
    ZeroShareProbe probe;
    std::vector<Bar> bars;
    for (int i = 0; i <= 6; ++i)
        bars.push_back(mk(100, i >= 4 ? 111 : 101, 99, 100, (i + 1) * 60000LL));
    if (!run_reservation(probe, bars, "withdraw zero share")) return;
    CHECK(probe.staged);
    int tp = 0;
    double rest = 0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const auto trade = probe.get_trade(i);
        if (trade.exit_id == "TP") {
            ++tp;
            CHECK(trade.exit_bar_index == 4 && near(trade.qty, 2) && near(trade.exit_price, 110));
        } else {
            CHECK(trade.exit_id == "__close__L" && trade.exit_bar_index == 6);
            rest += trade.qty;
        }
    }
    CHECK(tp == 1);
    CHECK(near(rest, 4) && near(probe.position(), 0));
}

} // namespace

int main(int argc, char** argv) {
    std::printf("=== test_exit_bracket_pending_entry_leg ===\n");

    const std::string row = argc == 2 ? argv[1] : "";
    if (argc > 2 || (!row.empty() && row != "controls"
                    && row != "percentage-retention" && row != "retention-cycle"
                    && row != "withdrawn-unfilled" && row != "percent-on-add"
                    && row != "percent-after-fill" && row != "unequal-percent-after-fill"
                    && row != "spent-tp-fill-order" && row != "zero-percent-share"
                    && row != "pending-add-shares")) {
        std::printf("unknown reservation row\n");
        return 2;
    }
    if (row.empty() || row == "controls") {
        test_reissue_over_pending_entry_arms_a_pair_per_instance();
        test_consumed_leg_and_capped_entry();
        test_rerun_reproduces_the_leg_census();
    }
    if (row.empty() || row == "percentage-retention")
        test_percentage_facts_survive_family_withdrawal();
    if (row.empty() || row == "retention-cycle")
        test_fact_only_family_expires_before_erasure();
    if (row.empty() || row == "withdrawn-unfilled")
        test_withdrawn_unfilled_origins_remain_eligible();
    if (row.empty() || row == "percent-on-add")
        test_percent_exit_on_add_fill_bar();
    if (row.empty() || row == "percent-after-fill")
        test_percent_add_after_first_take_profit();
    if (row.empty() || row == "unequal-percent-after-fill")
        test_percentage_reissue_after_fill_covers_only_the_add();
    if (row.empty() || row == "spent-tp-fill-order")
        test_spent_tp_fill_order();
    if (row.empty() || row == "zero-percent-share")
        test_zero_share_cannot_fill();
    if (row.empty() || row == "pending-add-shares")
        test_each_pending_add_owns_a_share();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    std::printf("setup failures: %d\n", setup_failed);
    if (setup_failed > 0) return 2;
    return (tests_failed > 0) ? 1 : 0;
}
