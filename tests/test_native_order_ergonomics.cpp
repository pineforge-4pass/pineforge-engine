// L7 pure-native witnesses for order ergonomics: the working-request
// snapshot, bulk cancellation, owner-fill anchored levels, tick-spelled and
// zero trailing offsets, retained trigger state on replace, and the toolkit
// bracket against the same legs submitted by hand. No generated source host
// or adapter participates.
#include <pineforge/native_toolkit.hpp>

#include "native_current_fixture.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace r4_test;
namespace tk = pineforge::native_toolkit;

namespace {

// One flat bar per price, exactly like the fixture's run helper, but kept
// local so a scenario can assert on the bar timestamps it drove.
std::vector<Bar> flat_bars(std::initializer_list<double> prices) {
    std::vector<Bar> bars;
    for (double price : prices) {
        bars.push_back({price, price, price, price, 1.0, T + int64_t(bars.size()) * 60000});
    }
    return bars;
}

double units_of(const no::RemainingProjection& remaining) {
    if (const auto* units = std::get_if<no::RemainingProjectionUnits>(&remaining)) return units->q;
    return -1.0;
}

const no::ExecutionAppliedEvent* applied_for(const std::vector<no::ExecutionAppliedEvent>& rows,
                                             const no::RequestHandle& handle) {
    for (const auto& row : rows) {
        if (row.definition && row.definition->handle == handle) return &row;
    }
    return nullptr;
}

// 1. The snapshot is exactly the live book, before and after a partial fill.
void working_snapshot_tracks_the_live_book() {
    struct SnapshotHost final : Host {
        no::RequestHandle entry, resting;
        std::vector<NativeWorkingRequest> at_open, after_fill;
        std::optional<no::RemainingProjection> last_applied, applied_when_snapshotted;
        double units_when_snapshotted = 0.0;
        int opens = 0;

        void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
            if (++opens == 1) {
                at_open = native_working_requests();
            } else if (opens == 2) {
                after_fill = native_working_requests();
                applied_when_snapshotted = last_applied;
                units_when_snapshotted = physical_position().signed_units;
            }
        }
        void on_native_applied(const no::ExecutionAppliedEvent& event,
                               const NativeDecisionContext& context) override {
            if (event.definition && event.definition->handle == entry) {
                last_applied = event.remaining_after;
            }
            Host::on_native_applied(event, context);
        }
    };

    SnapshotHost host;
    host.beginning = [&](Host& base) {
        auto entry = tx(3.0, "entry");
        entry.capacity = no::PointBudget{1.0};
        host.entry = put(base, entry);
        auto resting = tx(1.0, "resting");
        resting.trigger = no::Limit{50.0};
        host.resting = put(base, resting);
    };

    REQUIRE(host.configure_native(spec("l7-working")).status == NativeSetupStatus::Applied);
    const auto bars = flat_bars({100.0, 100.0});
    host.run(bars.data(), int(bars.size()));

    // Nothing has matched yet at the first open: both requests are working
    // for their whole submitted quantity.
    REQUIRE(host.at_open.size() == 2);
    CHECK(host.at_open[0].definition->handle == host.entry);
    CHECK(host.at_open[1].definition->handle == host.resting);
    near(units_of(host.at_open[0].remaining), 3.0);
    near(units_of(host.at_open[1].remaining), 1.0);
    CHECK(std::holds_alternative<no::MarketReady>(host.at_open[0].trigger_state));
    CHECK(std::holds_alternative<no::LimitReady>(host.at_open[1].trigger_state));

    // The per-point budget left working units behind, and the snapshot taken
    // at the next open reports exactly what the last receipt projected.
    REQUIRE(host.applied_when_snapshotted.has_value());
    const double working = units_of(*host.applied_when_snapshotted);
    CHECK(working > 0.0);
    REQUIRE(host.after_fill.size() == 2);
    CHECK(host.after_fill[0].definition->handle == host.entry);
    CHECK(host.after_fill[1].definition->handle == host.resting);
    near(units_of(host.after_fill[0].remaining), working);
    near(units_of(host.after_fill[1].remaining), 1.0);
    CHECK(std::holds_alternative<no::MarketReady>(host.after_fill[0].trigger_state));
    near(host.units_when_snapshotted, 3.0 - working);

    // The residual finishes, and a terminal request leaves the snapshot.
    CHECK(host.physical_position().signed_units == 3.0);
    const auto remaining_rows = host.native_working_requests();
    REQUIRE(remaining_rows.size() == 1);
    CHECK(remaining_rows[0].definition->handle == host.resting);
    completed(host);
}

// 2. cancel_where cancels exactly the matching comments; cancel_all empties
//    the book and emits one CancelledEvent per request.
void bulk_cancellation_is_exact() {
    Host host;
    no::RequestHandle keep_a, keep_b, drop;
    std::size_t where_count = 0, all_count = 0, live_after_where = 0, live_after_all = 0;
    std::size_t cancelled_after_where = 0;
    bool ran = false;
    host.beginning = [&](Host& base) {
        auto make = [&](const char* label, const char* comment) {
            no::Request request{no::Transact{1.0}, label, comment};
            request.trigger = no::Limit{50.0};
            return put(base, request);
        };
        keep_a = make("a", "keep");
        drop = make("b", "drop");
        keep_b = make("c", "keep");
    };
    host.calculation = [&](Host& base) {
        if (ran) return;
        ran = true;
        where_count = base.cancel_where("drop");
        live_after_where = base.native_working_requests().size();
        cancelled_after_where = events<no::CancelledEvent>(base).size();
        all_count = base.cancel_all();
        live_after_all = base.native_working_requests().size();
    };

    run(host, spec("l7-cancel"), {100.0, 100.0});

    CHECK(where_count == 1);
    CHECK(live_after_where == 2);
    CHECK(cancelled_after_where == 1);
    CHECK(all_count == 2);
    CHECK(live_after_all == 0);
    const auto cancelled = events<no::CancelledEvent>(host);
    REQUIRE(cancelled.size() == 3);
    CHECK(cancelled[0].handle() == drop);
    CHECK(cancelled[0].request().comment == "drop");
    CHECK(cancelled[1].handle() == keep_a);
    CHECK(cancelled[2].handle() == keep_b);
    for (const auto& row : cancelled) CHECK(row.reason == no::CancelReason::User);
    CHECK(host.cancel_all() == 0);
    completed(host);
}

// 3. A tick-spelled anchor becomes fill + offset * tick at the arm.
void anchored_stop_arms_at_the_owner_fill() {
    Host host;
    no::RequestHandle parent, leg;
    host.beginning = [&](Host& base) {
        parent = put(base, tx(1.0, "parent"));
        no::Request stop{no::Reduce{no::OwnerOpenedUnits{}}, "stop", ""};
        stop.owner = no::WaitForApplied{parent};
        stop.trigger = no::Stop{0.0};
        stop.anchor = no::FromOwnerFill{-10.0, true};
        leg = put(base, stop);
    };

    run(host, spec("l7-anchor"), {100.0, 99.8});

    const auto armed = events<no::ArmedEvent>(host);
    REQUIRE(armed.size() == 1);
    REQUIRE(armed[0].definition && armed[0].definition->handle == leg);
    const auto* level = std::get_if<no::Stop>(&armed[0].definition->request.trigger);
    REQUIRE(level != nullptr);
    near(level->price, 99.9);
    // The materialized definition reads as the absolute level it now is.
    CHECK(std::holds_alternative<no::Absolute>(armed[0].definition->request.anchor));

    const auto applied = events<no::ExecutionAppliedEvent>(host);
    const auto* entry = applied_for(applied, parent);
    const auto* exit = applied_for(applied, leg);
    REQUIRE(entry != nullptr && exit != nullptr);
    near(entry->resolved_price, 100.0);
    near(exit->resolved_price, 99.8);
    near(exit->closed_units, 1.0);
    CHECK(host.physical_position().signed_units == 0.0);
    completed(host);
}

// A resting trail exit over one entry, used by the trailing scenarios.
struct TrailHost final : Host {
    no::Trail trail{};
    no::RequestHandle entry, exit;
    void arm() {
        beginning = [this](Host& base) {
            entry = put(base, tx(1.0, "entry"));
            no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "trail", ""};
            request.trigger = trail;
            exit = put(base, request);
        };
    }
};

// 4. A tick-spelled offset is the price-spelled offset, bar for bar.
void tick_offset_matches_the_price_offset() {
    const auto bars = flat_bars({100.0, 101.0, 102.0, 101.9});

    TrailHost priced;
    priced.trail = no::Trail{0.02, 100.5};
    priced.arm();
    REQUIRE(priced.configure_native(spec("l7-trail-ticks")).status == NativeSetupStatus::Applied);
    priced.run(bars.data(), int(bars.size()));

    TrailHost ticked;
    ticked.trail = no::Trail{0.0, 100.5, no::TrailTicks{2.0}};
    ticked.arm();
    REQUIRE(ticked.configure_native(spec("l7-trail-ticks")).status == NativeSetupStatus::Applied);
    ticked.run(bars.data(), int(bars.size()));

    // The spelling is resolved at acceptance, so the accepted request is the
    // price-spelled one and nothing downstream can tell the two runs apart.
    const auto accepted = events<no::AcceptedEvent>(ticked);
    REQUIRE(accepted.size() == 2);
    const auto* resolved = std::get_if<no::Trail>(&accepted[1].request().trigger);
    REQUIRE(resolved != nullptr);
    CHECK(!resolved->ticks.has_value());
    CHECK(resolved->offset == 0.02);

    const auto priced_applied = events<no::ExecutionAppliedEvent>(priced);
    const auto ticked_applied = events<no::ExecutionAppliedEvent>(ticked);
    REQUIRE(priced_applied.size() == 2);
    REQUIRE(ticked_applied.size() == priced_applied.size());
    for (std::size_t i = 0; i < priced_applied.size(); ++i) {
        CHECK(priced_applied[i].ordinal == ticked_applied[i].ordinal);
        CHECK(priced_applied[i].resolved_price == ticked_applied[i].resolved_price);
        CHECK(priced_applied[i].closed_units == ticked_applied[i].closed_units);
        CHECK(priced_applied[i].opened_units == ticked_applied[i].opened_units);
    }
    near(priced_applied[1].resolved_price, 101.9);
    CHECK(priced.native_continuation_hash() == ticked.native_continuation_hash());
    completed(priced);
    completed(ticked);
}

// 5. A zero offset rides the best and exits on the first adverse move.
void zero_offset_trail_rides_the_best() {
    TrailHost host;
    host.trail = no::Trail{0.0, 100.5};
    host.arm();
    REQUIRE(host.configure_native(spec("l7-trail-zero")).status == NativeSetupStatus::Applied);
    const auto bars = flat_bars({100.0, 101.0, 102.0, 101.99});
    host.run(bars.data(), int(bars.size()));

    const auto applied = events<no::ExecutionAppliedEvent>(host);
    REQUIRE(applied.size() == 2);
    const auto* exit = applied_for(applied, host.exit);
    REQUIRE(exit != nullptr);
    // Not at the arm bar and not at the improving bar: the first adverse
    // print past the running best is the exit.
    near(exit->resolved_price, 101.99);
    CHECK(exit->cursor.point.interval_index == 3);
    CHECK(host.physical_position().signed_units == 0.0);
    completed(host);

    // A negative offset is still not a trail.
    Host rejects;
    no::SubmitResult negative;
    rejects.beginning = [&](Host& base) {
        no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "negative", ""};
        request.trigger = no::Trail{-1.0, 100.5};
        negative = base.submit(request);
    };
    run(rejects, spec("l7-trail-negative"), {100.0});
    CHECK(negative.status == no::SubmitStatus::Rejected);
    REQUIRE(negative.reason.has_value());
    CHECK(*negative.reason == no::RequestRejectReason::InvalidTrigger);
}

// 6. Replacement keeps the running extreme only when asked to.
void replace_retains_trigger_state_on_request() {
    auto probe = [](bool retain) {
        TrailHost host;
        host.trail = no::Trail{0.02, 100.5};
        host.arm();
        std::optional<NativeTrailState> after_replace;
        bool replaced = false;
        host.calculation = [&](Host& base) {
            if (replaced || base.native_working_requests().empty()) return;
            const auto before = base.trail_state(host.exit);
            if (!before || !before->activated || before->best_price < 102.0) return;
            replaced = true;
            no::Request successor{no::Reduce{no::ExplicitUnits{1.0}}, "trail", ""};
            successor.trigger = no::Trail{0.02, 100.5};
            const auto result = base.replace(host.exit, successor,
                                             no::ReplaceOptions{retain});
            REQUIRE(result.status == no::ReplaceStatus::Replaced && result.successor);
            host.exit = *result.successor;
            after_replace = base.trail_state(host.exit);
        };
        REQUIRE(host.configure_native(spec("l7-trail-retain")).status
                == NativeSetupStatus::Applied);
        const auto bars = flat_bars({100.0, 101.0, 102.0, 102.0});
        host.run(bars.data(), int(bars.size()));
        CHECK(replaced);
        completed(host);
        return after_replace;
    };

    const auto retained = probe(true);
    REQUIRE(retained.has_value());
    CHECK(retained->activated);
    near(retained->best_price, 102.0);
    near(retained->current_level, 101.98);

    const auto reset = probe(false);
    REQUIRE(reset.has_value());
    CHECK(!reset->activated);
    CHECK(reset->best_price == 0.0);
    CHECK(reset->current_level == 0.0);
}

// 7. TWIN: the builder emits exactly the legs a host would submit by hand.
void submit_bracket_twins_the_hand_written_legs() {
    const auto bars = flat_bars({100.0, 101.0, 102.0});
    auto legs = [] {
        std::vector<no::Request> rows;
        no::Request take_profit{no::Reduce{no::OwnerOpenedUnits{}}, "tp", "bracket"};
        take_profit.trigger = no::Limit{102.0};
        no::Request stop_loss{no::Reduce{no::OwnerOpenedUnits{}}, "sl", "bracket"};
        stop_loss.trigger = no::Stop{98.0};
        no::Request trail{no::Reduce{no::OwnerOpenedUnits{}}, "tr", "bracket"};
        trail.trigger = no::Trail{0.02, 101.0};
        rows.push_back(std::move(take_profit));
        rows.push_back(std::move(stop_loss));
        rows.push_back(std::move(trail));
        return rows;
    }();

    Host built;
    tk::BracketReceipt receipt;
    built.beginning = [&](Host& base) {
        tk::BracketSpec spec_rows;
        spec_rows.parent = put(base, tx(1.0, "entry"));
        spec_rows.take_profit = legs[0];
        spec_rows.stop_loss = legs[1];
        spec_rows.trail = legs[2];
        receipt = tk::submit_bracket(base, spec_rows);
    };
    REQUIRE(built.configure_native(spec("l7-bracket")).status == NativeSetupStatus::Applied);
    built.run(bars.data(), int(bars.size()));

    Host manual;
    manual.beginning = [&](Host& base) {
        const auto parent = put(base, tx(1.0, "entry"));
        std::int64_t cohort = 1;
        for (const auto& leg : legs) {
            no::Request request = leg;
            request.owner = no::WaitForApplied{parent};
            request.group = no::Member{parent.incarnation, cohort++,
                                       no::GroupEffect::Cancel};
            put(base, request);
        }
    };
    REQUIRE(manual.configure_native(spec("l7-bracket")).status == NativeSetupStatus::Applied);
    manual.run(bars.data(), int(bars.size()));

    REQUIRE(receipt.take_profit && receipt.stop_loss && receipt.trail);
    const auto built_events = built.native_events(0);
    const auto manual_events = manual.native_events(0);
    REQUIRE(built_events.size() == manual_events.size());
    for (std::size_t i = 0; i < built_events.size(); ++i) {
        CHECK(built_events[i].kind == manual_events[i].kind);
        CHECK(built_events[i].ordinal == manual_events[i].ordinal);
        CHECK(built_events[i].command.has_value() == manual_events[i].command.has_value());
        if (built_events[i].command && manual_events[i].command) {
            CHECK(built_events[i].command->index() == manual_events[i].command->index());
        }
    }
    CHECK(built.native_continuation_hash() == manual.native_continuation_hash());

    // The probe really exercised the bracket: the entry filled, the limit leg
    // took the profit, and its siblings left with the group.
    const auto applied = events<no::ExecutionAppliedEvent>(built);
    REQUIRE(applied.size() == 2);
    near(applied[1].resolved_price, 102.0);
    const auto cancelled = events<no::CancelledEvent>(built);
    REQUIRE(cancelled.size() == 2);
    for (const auto& row : cancelled) CHECK(row.reason == no::CancelReason::Group);
    CHECK(built.native_working_requests().empty());
    completed(built);
    completed(manual);
}

}  // namespace

int main() {
    test("working snapshot tracks the live book", working_snapshot_tracks_the_live_book);
    test("bulk cancellation is exact", bulk_cancellation_is_exact);
    test("anchored stop arms at the owner fill", anchored_stop_arms_at_the_owner_fill);
    test("tick offset matches the price offset", tick_offset_matches_the_price_offset);
    test("zero offset trail rides the best", zero_offset_trail_rides_the_best);
    test("replace retains trigger state on request", replace_retains_trigger_state_on_request);
    test("submit_bracket twins the hand written legs",
         submit_bracket_twins_the_hand_written_legs);
    std::printf("L7 native order ergonomics: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
