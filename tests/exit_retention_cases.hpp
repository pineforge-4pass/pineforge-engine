// Included by test_exit_bracket_pending_entry_leg.cpp after its native fixtures.
// Read-only member access follows the existing PrivateMember test pattern.
template<class Tag, typename Tag::Type Member>
struct RetentionMember {
    friend typename Tag::Type retention_member(Tag) { return Member; }
};

struct RetentionFamilies {
    using Type = std::unordered_map<std::uint64_t, source::BracketRoster>
        source::PineExecutionAdapter::*;
    friend Type retention_member(RetentionFamilies);
};
template struct RetentionMember<RetentionFamilies,
    &source::PineExecutionAdapter::bracket_families_>;

struct RetentionArchive {
    using Type = std::unordered_map<std::uint64_t, source::BracketRoster::Settled>
        source::PineExecutionAdapter::*;
    friend Type retention_member(RetentionArchive);
};
template struct RetentionMember<RetentionArchive,
    &source::PineExecutionAdapter::settled_bracket_families_>;

class RetentionProbe final : public PartialReservationProbe {
public:
    enum class Withdrawal { All, AllNa, One };
    enum class Scenario { Reissue, CycleEnd, Unfilled };
    struct FamilyState {
        std::size_t active = 0, archived = 0, members = 0, facts = 0;
        bool empty = true, wholly_settled = true;
        std::array<bool, 3> old_consumed{{false, false, false}};
    };
    using Snapshot = source::PineExecutionAdapter::FixturePendingSnapshot;
    static constexpr double entry_units = 0.02;
    static constexpr double first_units = entry_units * 0.5;
    static constexpr double renewed_units = entry_units * 0.75;

    RetentionProbe(bool is_long, bool any, double grid, int pyramiding,
                   Withdrawal withdrawal, Scenario scenario, bool fresh = false,
                   bool reverse = false, bool units = false)
        : PartialReservationProbe(configuration(any, pyramiding), 0.01, grid),
          is_long_(is_long), withdrawal_(withdrawal), scenario_(scenario),
          fresh_(fresh), reverse_(reverse), units_(units) {}

    FamilyState after_cancel, after_reissue, while_askable, after_boundary;
    std::vector<Snapshot> initial_legs, renewed_legs, fresh_legs;
    double after_retouch = kNaN, boundary_position = kNaN;
    int slice_events = 0;
    int configured_pyramiding() const { return fixture_configuration().pyramiding; }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("seed", is_long_, kNaN, kNaN, entry_units);
        if (bar_index_ == 1) {
            arm(is_long_, 50.0, scenario_ == Scenario::Unfilled);
            initial_legs = slice_legs();
        }
        if (bar_index_ == 3) {
            withdraw();
            after_cancel = family_state();
            if (scenario_ != Scenario::CycleEnd) {
                arm(is_long_, 75.0);
                after_reissue = family_state();
                renewed_legs = slice_legs();
            }
        }
        if (bar_index_ == 4 && scenario_ == Scenario::CycleEnd)
            while_askable = family_state();
        if (bar_index_ == 5) {
            after_retouch = position();
            if (scenario_ == Scenario::CycleEnd) {
                if (reverse_)
                    strategy_entry("seed", !is_long_, kNaN, kNaN, entry_units);
                else strategy_close("seed");
            }
        }
        if (bar_index_ == 6) {
            if (scenario_ == Scenario::Reissue && fresh_) {
                strategy_entry("seed", is_long_, kNaN, kNaN, entry_units);
                arm(is_long_, 75.0);
            }
            if (scenario_ == Scenario::CycleEnd) {
                after_boundary = family_state();
                boundary_position = position();
            }
        }
        if (bar_index_ == 7) {
            if (scenario_ == Scenario::Reissue && fresh_) {
                arm(is_long_, 75.0);
                fresh_legs = slice_legs();
            } else if (scenario_ == Scenario::CycleEnd) {
                if (!reverse_)
                    strategy_entry("seed", !is_long_, kNaN, kNaN, entry_units);
                arm(!is_long_, 50.0);
            } else if (scenario_ == Scenario::Unfilled) {
                strategy_close("seed");
            }
        }
        if (bar_index_ == 8 && scenario_ == Scenario::CycleEnd)
            fresh_legs = slice_legs();
        if (bar_index_ == 9 && scenario_ == Scenario::Reissue)
            strategy_close("seed");
        if (bar_index_ == 10 && scenario_ == Scenario::CycleEnd)
            strategy_close("seed");
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                const NativeDecisionContext& context) override {
        PartialReservationProbe::on_native_applied_event(event, context);
        if (event.closed_units > 0.0 && event.request().label == "slice")
            ++slice_events;
    }

private:
    static source::PineStrategyConfig configuration(bool any, int pyramiding) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = entry_units;
        config.pyramiding = pyramiding;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.close_entries_rule_any = any;
        return config;
    }
    void arm(bool is_long, double percent, bool unreachable = false) {
        strategy_exit("slice", "seed",
                      unreachable ? (is_long ? 150.0 : 50.0) : (is_long ? 110.0 : 90.0),
                      kNaN, kNaN, kNaN, kNaN, units_ ? 100.0 : percent,
                      "", units_ ? first_units : kNaN);
    }
    void withdraw() {
        if (withdrawal_ == Withdrawal::All) strategy_cancel_all();
        else if (withdrawal_ == Withdrawal::One) strategy_cancel("slice");
        else strategy_exit("slice", "seed", kNaN, kNaN, kNaN, kNaN, kNaN,
                           units_ ? 100.0 : 50.0, "", units_ ? first_units : kNaN);
    }
    FamilyState family_state() const {
        FamilyState state;
        const auto& active = adapter_.*retention_member(RetentionFamilies{});
        const auto& archived = adapter_.*retention_member(RetentionArchive{});
        state.active = active.size();
        state.archived = archived.size();
        for (const auto& item : active) {
            const auto& family = item.second;
            state.members += family.size();
            state.facts += family.consumed_facts().size();
            state.empty = state.empty && family.empty();
            state.wholly_settled = state.wholly_settled && family.wholly_settled();
            if (!openings.empty()) {
                const auto origin = openings.front().origin;
                state.old_consumed[0] = family.consumed(
                    static_cast<std::uint8_t>(source::PineOrderFamily::ExitLimit), origin);
                state.old_consumed[1] = family.consumed(
                    static_cast<std::uint8_t>(source::PineOrderFamily::ExitStop), origin);
                state.old_consumed[2] = family.consumed(
                    static_cast<std::uint8_t>(source::PineOrderFamily::ExitTrail), origin);
            }
        }
        return state;
    }
    std::vector<Snapshot> slice_legs() const {
        auto rows = adapter_.fixture_pending_snapshots();
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.snapshot.source_id != "slice";
        }), rows.end());
        return rows;
    }
    bool is_long_;
    Withdrawal withdrawal_;
    Scenario scenario_;
    bool fresh_, reverse_, units_;
};

static void check_retention_fact_only(const RetentionProbe::FamilyState& state) {
    CHECK(state.active == 1);
    CHECK(state.archived == 0);
    CHECK(state.members == 0);
    CHECK(state.facts == 3);
    CHECK(!state.empty);
    CHECK(!state.wholly_settled);
    for (const bool consumed : state.old_consumed) CHECK(consumed);
}

static void check_retention_grid(RetentionProbe& probe, double grid, int pyramiding) {
    const auto state = probe.native_state();
    CHECK_SETUP(state.spec != nullptr);
    if (!state.spec) return;
    CHECK_SETUP(state.spec->quantity_grid.has_value() == (grid > 0.0));
    if (grid > 0.0)
        CHECK_SETUP(state.spec->quantity_grid && near(*state.spec->quantity_grid, grid, 1e-12));
    CHECK_SETUP(probe.configured_pyramiding() == pyramiding);
    CHECK_SETUP(!probe.openings.empty());
    for (const auto& opening : probe.openings) {
        CHECK_SETUP(opening.origin != 0);
        CHECK_SETUP(opening.id == "seed");
        CHECK_SETUP(near(opening.units, RetentionProbe::entry_units));
    }
}

static std::vector<Bar> retention_bars(bool is_long, RetentionProbe::Scenario scenario) {
    std::vector<Bar> bars;
    for (int index = 0; index <= 12; ++index) {
        auto bar = mk(100, 101, 99, 100, (index + 1) * 60000LL);
        const bool initial_touch = scenario != RetentionProbe::Scenario::Unfilled && index == 2;
        const bool repeated_touch = scenario == RetentionProbe::Scenario::Reissue
            && (index == 4 || index == 5 || index == 8);
        const bool replacement_touch = scenario == RetentionProbe::Scenario::Unfilled && index == 5;
        if (initial_touch || repeated_touch || replacement_touch) {
            if (is_long) bar.high = 111;
            else bar.low = 89;
        }
        if (scenario == RetentionProbe::Scenario::CycleEnd && index == 9) {
            if (is_long) bar.low = 89;
            else bar.high = 111;
        }
        bars.push_back(bar);
    }
    return bars;
}

static void test_percentage_facts_survive_family_withdrawal() {
    using Withdrawal = RetentionProbe::Withdrawal;
    using Scenario = RetentionProbe::Scenario;
    struct Route { int pyramiding; bool fresh; };
    // Under pyramiding above one, a named percentage retains per-opening
    // facts only through the per-origin percentage sizing, which this line
    // does not carry; its routes Route{2, false} and Route{2, true} return
    // with that sizing.
    for (const bool is_long : {true, false})
        for (const bool any : {false, true})
            for (const double grid : {0.0, 0.00001})
                for (const auto withdrawal : {Withdrawal::All, Withdrawal::AllNa})
                    for (const auto route : {Route{1, false}}) {
                        RetentionProbe probe(is_long, any, grid, route.pyramiding,
                                             withdrawal, Scenario::Reissue, route.fresh);
                        if (!run_reservation(probe, retention_bars(is_long, Scenario::Reissue),
                                             "percentage-family-retention")) continue;
                        check_retention_grid(probe, grid, route.pyramiding);
                        CHECK(probe.openings.size() == (route.fresh ? 2u : 1u));
                        for (std::size_t index = 0; index < probe.openings.size(); ++index)
                            CHECK_SETUP(probe.openings[index].bar == (index == 0 ? 1 : 7));
                        if (probe.openings.size() == 2)
                            CHECK_SETUP(probe.openings[0].origin != probe.openings[1].origin);
                        CHECK(!probe.initial_legs.empty());
                        for (const auto& leg : probe.initial_legs) {
                            CHECK(std::isnan(leg.snapshot.requested_qty));
                            CHECK(near(leg.snapshot.projection_remaining_qty, RetentionProbe::first_units));
                        }
                        if (route.pyramiding == 2) {
                            check_retention_fact_only(probe.after_cancel);
                            check_retention_fact_only(probe.after_reissue);
                        }
                        CHECK(probe.renewed_legs.empty());
                        CHECK(near(probe.after_retouch, is_long ? 0.01 : -0.01));
                        if (route.fresh) {
                            CHECK(probe.fresh_legs.size() == 1);
                            for (const auto& leg : probe.fresh_legs) {
                                CHECK(std::isnan(leg.snapshot.requested_qty));
                                CHECK(probe.openings.size() == 2
                                    && leg.snapshot.bracket_origin.incarnation == probe.openings[1].origin);
                                CHECK(near(leg.snapshot.projection_remaining_qty, RetentionProbe::renewed_units));
                            }
                        }
                        const int expected_slice_rows = route.fresh ? (any ? 2 : 3) : 1;
                        const int expected_close_rows = route.fresh && any ? 2 : 1;
                        int slice_rows = 0, close_rows = 0;
                        double slice_units = 0.0, close_units = 0.0;
                        for (int index = 0; index < probe.trade_count(); ++index) {
                            const auto& trade = probe.get_trade(index);
                            CHECK(trade.entry_id == "seed");
                            CHECK(trade.entry_bar_index == 1 || (route.fresh && trade.entry_bar_index == 7));
                            CHECK(near(trade.entry_price, 100.0));
                            if (trade.exit_id == "slice") {
                                CHECK(trade.exit_bar_index == (slice_rows == 0 ? 2 : 8));
                                CHECK(near(trade.exit_price, is_long ? 110.0 : 90.0));
                                const double expected = slice_rows == 0 ? 0.01
                                    : any ? 0.015 : slice_rows == 1 ? 0.01 : 0.005;
                                CHECK(near(trade.qty, expected));
                                CHECK(trade.entry_bar_index
                                    == (slice_rows == 0 || (!any && slice_rows == 1) ? 1 : 7));
                                slice_units += trade.qty;
                                ++slice_rows;
                            } else {
                                CHECK(trade.exit_id == "__close__seed");
                                CHECK(trade.exit_bar_index == 10);
                                CHECK(near(trade.exit_price, 100.0));
                                CHECK(near(trade.qty, !route.fresh ? 0.01
                                    : any ? (close_rows == 0 ? 0.01 : 0.005) : 0.015));
                                close_units += trade.qty;
                                ++close_rows;
                            }
                        }
                        CHECK(slice_rows == expected_slice_rows);
                        CHECK(close_rows == expected_close_rows);
                        CHECK(probe.trade_count() == (route.fresh ? 4 : 2));
                        CHECK(probe.slice_events == (route.fresh ? 2 : 1));
                        CHECK(near(slice_units, route.fresh ? 0.025 : 0.01));
                        CHECK(near(close_units, route.fresh ? 0.015 : 0.01));
                        CHECK(near(probe.position(), 0.0));
                    }
}

static void test_fact_only_family_expires_before_erasure() {
    using Withdrawal = RetentionProbe::Withdrawal;
    using Scenario = RetentionProbe::Scenario;
    // Pyramiding is two here, so the percentage rows (units false) need the
    // per-origin percentage sizing, which this line does not carry; they
    // return with it. The explicit-unit rows stay.
    for (const bool is_long : {true, false})
        for (const bool any : {false, true})
            for (const double grid : {0.0, 0.00001})
                for (const auto withdrawal : {Withdrawal::All, Withdrawal::AllNa})
                    for (const bool reverse : {false, true})
                        for (const bool units : {true}) {
                            RetentionProbe probe(is_long, any, grid, 2, withdrawal,
                                                 Scenario::CycleEnd, false, reverse, units);
                            if (!run_reservation(probe, retention_bars(is_long, Scenario::CycleEnd),
                                                 "fact-only-cycle-end")) continue;
                            check_retention_grid(probe, grid, 2);
                            CHECK(probe.openings.size() == 2);
                            for (std::size_t index = 0; index < probe.openings.size(); ++index)
                                CHECK_SETUP(probe.openings[index].bar
                                    == (index == 0 ? 1 : reverse ? 6 : 8));
                            if (probe.openings.size() == 2)
                                CHECK_SETUP(probe.openings[0].origin != probe.openings[1].origin);
                            check_retention_fact_only(probe.after_cancel);
                            check_retention_fact_only(probe.while_askable);
                            CHECK(probe.after_boundary.active == 0);
                            CHECK(probe.after_boundary.archived == 0);
                            CHECK(probe.after_boundary.facts == 0);
                            CHECK(near(probe.boundary_position, reverse
                                ? (is_long ? -0.02 : 0.02) : 0.0));
                            CHECK(probe.fresh_legs.size() == 1);
                            for (const auto& leg : probe.fresh_legs) {
                                CHECK(probe.openings.size() == 2
                                    && leg.snapshot.bracket_origin.incarnation == probe.openings[1].origin);
                                CHECK(units ? std::isfinite(leg.snapshot.requested_qty)
                                            : std::isnan(leg.snapshot.requested_qty));
                                CHECK(near(leg.snapshot.projection_remaining_qty, 0.01));
                            }
                            CHECK(probe.trade_count() == 4);
                            CHECK(probe.slice_events == 2);
                            for (int index = 0; index < probe.trade_count(); ++index) {
                                const auto& trade = probe.get_trade(index);
                                CHECK(trade.entry_id == "seed");
                                CHECK(trade.entry_bar_index == (index < 2 ? 1 : reverse ? 6 : 8));
                                CHECK(trade.exit_bar_index == (index == 0 ? 2 : index == 1 ? 6
                                                                            : index == 2 ? 9 : 11));
                                CHECK(near(trade.qty, 0.01));
                                CHECK(trade.is_long == (index < 2 ? is_long : !is_long));
                                CHECK(trade.exit_id == (index == 0 || index == 2 ? "slice"
                                    : index == 1 && reverse ? "seed" : "__close__seed"));
                                CHECK(near(trade.exit_price, index == 0 ? (is_long ? 110.0 : 90.0)
                                    : index == 2 ? (is_long ? 90.0 : 110.0) : 100.0));
                            }
                            CHECK(near(probe.position(), 0.0));
                        }
}

static void test_withdrawn_unfilled_origins_remain_eligible() {
    using Withdrawal = RetentionProbe::Withdrawal;
    using Scenario = RetentionProbe::Scenario;
    for (const bool is_long : {true, false})
        for (const bool any : {false, true})
            for (const double grid : {0.0, 0.00001})
                for (const int pyramiding : {1, 2})
                    for (const auto withdrawal : {Withdrawal::All, Withdrawal::AllNa, Withdrawal::One})
                        for (const bool units : {false, true}) {
                            RetentionProbe probe(is_long, any, grid, pyramiding, withdrawal,
                                                 Scenario::Unfilled, false, false, units);
                            if (!run_reservation(probe, retention_bars(is_long, Scenario::Unfilled),
                                                 "withdrawn-unfilled")) continue;
                            check_retention_grid(probe, grid, pyramiding);
                            CHECK(probe.openings.size() == 1);
                            for (const auto& opening : probe.openings) CHECK_SETUP(opening.bar == 1);
                            CHECK(probe.after_cancel.facts == 0);
                            for (const bool consumed : probe.after_cancel.old_consumed) CHECK(!consumed);
                            CHECK(probe.renewed_legs.size() == 1);
                            CHECK(probe.trade_count() == 2);
                            CHECK(probe.slice_events == 1);
                            for (int index = 0; index < probe.trade_count(); ++index) {
                                const auto& trade = probe.get_trade(index);
                                CHECK(trade.entry_id == "seed");
                                CHECK(trade.entry_bar_index == 1);
                                CHECK(trade.exit_id == (index == 0 ? "slice" : "__close__seed"));
                                CHECK(trade.exit_bar_index == (index == 0 ? 5 : 8));
                                CHECK(near(trade.qty, units ? 0.01 : index == 0 ? 0.015 : 0.005));
                                CHECK(near(trade.exit_price, index == 0
                                    ? (is_long ? 110.0 : 90.0) : 100.0));
                            }
                            CHECK(near(probe.position(), 0.0));
                        }
}
