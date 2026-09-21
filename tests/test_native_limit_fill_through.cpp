// R5 gap lane P5 (GAP-T1 / FP1): the market-if-touched geometry of
// native_order::Limit{price, fill_through}, pinned from a bare host.
//
// A limit is two facts. The LEVEL gates when the request becomes executable:
// the modeled path has to reach it (a crossing on a segment books the level
// itself; a point already inside the region — a gap open — books that
// print). The BOUND caps the resolved fill at the level after slippage. A
// bounded limit (fill_through == false) has both; `fill_through` keeps the
// gate and drops the bound, so slippage — or a host's own terms — may carry
// the fill past the level. Every scenario below runs the same tape through
// two hosts that differ only in the flag and compares the two outcomes:
//
//   crossing  : level 98 reached on the path       raw 98,    bounded 98,  through 98.5
//   gap open  : open 97.75 already inside            raw 97.75, bounded 98,  through 98.25
//   no touch  : low 98.25 never reaches the level    no fill either way
//   zero slip : nothing to carry                     both 98, same book
//   host terms: resolve_execution_terms answers 99   bounded InvalidTerms, through fills 99
//
// with the sell side mirrored around a 102 level. The tick is 0.25 and the
// slippage two ticks, so every expected price is an exact binary64 and is
// compared with ==. The flag is durable request state and is folded into the
// continuation identity only when set, so hosts that book the same fill still
// carry different identities.
//
// Nothing here reaches a source or compat header: this TU builds and runs in
// the kernel-only profile.
#include "native_current_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace r4_test;

namespace {

Bar ohlc(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, T + static_cast<int64_t>(index) * 60000};
}

// A host that submits one priced opening at its first calculation and keeps
// every terms consultation the kernel makes, so the candidate's price kind
// (the level, or the print) and the kernel's default resolved price are
// visible. `terms_offset` makes it answer a price of its own.
struct LimitHost : Host {
    double units = 0.0;
    no::Limit limit;
    std::optional<double> terms_offset;
    mutable std::vector<NativeExecutionTermsFacts> consulted;

    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        consulted.push_back(facts);
        no::ExecutionTerms terms = Host::resolve_execution_terms(facts);
        if (terms_offset) terms.resolved_price = facts.default_resolved_price + *terms_offset;
        return terms;
    }
};

NativeRunSpec limit_spec(const char* key, std::uint32_t slippage_ticks) {
    NativeRunSpec s = spec(key, 0.0);
    s.price_tick = 0.25;
    s.slippage_ticks = slippage_ticks;
    return s;
}

// Run one tape: a flat bar at 100 whose calculation submits the limit, then
// `second`, whose path is what the limit meets.
void drive(LimitHost& h, const NativeRunSpec& s, const Bar& second) {
    h.calculation = [](Host& host) {
        auto& self = static_cast<LimitHost&>(host);
        if (self.calculations - 1 != 0) return;
        no::Request request = tx(self.units, "touch");
        request.trigger = self.limit;
        put(self, request);
    };
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0), second};
    REQUIRE(h.configure_native(s).status == NativeSetupStatus::Applied);
    h.run(bars.data(), static_cast<int>(bars.size()));
    completed(h);
}

struct Side {
    const char* name;
    double units;      // +1 buys, -1 sells
    double level;
    Bar crossing;      // reaches the level on the path, opens on the far side
    Bar gap;           // opens one tick inside the region
    Bar untouched;     // extreme one tick short of the level
    double raw_gap;    // the gap open
    double carried;    // level moved one slip (0.5) past itself
    double gap_carried;
};

const Side kSides[] = {
    {"buy", +1.0, 98.0,
     ohlc(1, 100.0, 101.0, 95.0, 99.0),
     ohlc(1, 97.75, 99.0, 97.5, 98.5),
     ohlc(1, 100.0, 101.0, 98.25, 99.0),
     97.75, 98.5, 98.25},
    {"sell", -1.0, 102.0,
     ohlc(1, 100.0, 105.0, 99.0, 101.0),
     ohlc(1, 102.25, 102.5, 101.0, 101.5),
     ohlc(1, 100.0, 101.75, 99.0, 101.0),
     102.25, 101.5, 101.75},
};

struct Outcome {
    std::vector<no::ExecutionAppliedEvent> fills;
    std::vector<no::MatchRejectedEvent> rejects;
    std::vector<NativeExecutionTermsFacts> consulted;
    std::vector<NativeOpenLot> lots;
    NativePhysicalPosition position;
    std::size_t working = 0;
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
};

Outcome run_side(const Side& side, const Bar& second, bool fill_through,
                 std::uint32_t slippage_ticks, std::optional<double> terms_offset = std::nullopt) {
    LimitHost h;
    h.units = side.units;
    h.limit = no::Limit{side.level, fill_through};
    h.terms_offset = terms_offset;
    const std::string key = std::string("p5-fill-through-") + side.name;
    drive(h, limit_spec(key.c_str(), slippage_ticks), second);
    Outcome out;
    out.fills = events<no::ExecutionAppliedEvent>(h);
    out.rejects = events<no::MatchRejectedEvent>(h);
    out.consulted = h.consulted;
    out.lots = h.native_open_lots(second.close);
    out.position = h.physical_position();
    out.working = h.native_working_requests().size();
    out.continuation = h.native_continuation_hash();
    out.broker = h.broker_state_hash();
    return out;
}

// One fill, from the request the host submitted, booked at `resolved` from
// the modeled `raw` price; the book holds exactly that lot at that price.
void one_fill(const Outcome& out, const Side& side, double raw, double resolved,
              no::NativeCandidatePriceKind kind) {
    REQUIRE(out.fills.size() == 1);
    const auto& fill = out.fills[0];
    CHECK(fill.request().label == "touch");
    CHECK(fill.raw_price == raw);
    CHECK(fill.resolved_price == resolved);
    near(fill.opened_units, side.units);
    CHECK(fill.closed_units == 0.0);
    CHECK(out.rejects.empty());
    CHECK(out.working == 0);
    REQUIRE(out.consulted.size() == 1);
    const auto& facts = out.consulted[0];
    CHECK(facts.price_kind == kind);
    CHECK(facts.is_buy == (side.units > 0.0));
    CHECK(facts.raw_price == raw);
    CHECK(facts.trigger_level.has_value() && *facts.trigger_level == side.level);
    CHECK(out.position.lot_count == 1);
    CHECK(out.position.signed_units == side.units);
    CHECK(out.position.average_price == resolved);
    REQUIRE(out.lots.size() == 1);
    CHECK(out.lots[0].entry_price == resolved);
    CHECK(out.lots[0].signed_units == side.units);
    CHECK(out.lots[0].entry_label == "touch");
}

// 1. A crossing books the level as the modeled price. Bounded, the slipped
//    fill is capped back at the level; fill-through carries it one slip past.
//    The kernel's own default already knows the bound: the host is consulted
//    with 98 under the bound and with 98.5 without it.
void crossing_is_capped_or_carried() {
    for (const auto& side : kSides) {
        scenario = side.name;
        const Outcome bounded = run_side(side, side.crossing, false, 2);
        const Outcome through = run_side(side, side.crossing, true, 2);
        one_fill(bounded, side, side.level, side.level, no::NativeCandidatePriceKind::TriggerLevel);
        one_fill(through, side, side.level, side.carried, no::NativeCandidatePriceKind::TriggerLevel);
        CHECK(bounded.consulted[0].default_resolved_price == side.level);
        CHECK(through.consulted[0].default_resolved_price == side.carried);
        // The flag is durable request state: the same tape, a different
        // identity, and a different book.
        CHECK(bounded.continuation != through.continuation);
        CHECK(bounded.broker != through.broker);
    }
}

// 2. A point already inside the region — the bar opens past the level — books
//    the print itself, not the level, on both hosts. Slippage then carries the
//    print one slip further: the bound catches it at the level, fill-through
//    lets it settle 0.25 beyond.
void gap_open_books_the_print() {
    for (const auto& side : kSides) {
        scenario = side.name;
        const Outcome bounded = run_side(side, side.gap, false, 2);
        const Outcome through = run_side(side, side.gap, true, 2);
        one_fill(bounded, side, side.raw_gap, side.level, no::NativeCandidatePriceKind::PointPrice);
        one_fill(through, side, side.raw_gap, side.gap_carried,
                 no::NativeCandidatePriceKind::PointPrice);
        CHECK(bounded.consulted[0].default_resolved_price == side.level);
        CHECK(through.consulted[0].default_resolved_price == side.gap_carried);
        CHECK(bounded.continuation != through.continuation);
    }
}

// 3. The level still gates: a path that stops one tick short of it fills
//    nothing on either host, consults no terms, and leaves the request
//    working. The two books are the same empty book; the identities differ,
//    because the flag is folded whether or not it ever mattered.
void untouched_level_gates_both() {
    for (const auto& side : kSides) {
        scenario = side.name;
        const Outcome bounded = run_side(side, side.untouched, false, 2);
        const Outcome through = run_side(side, side.untouched, true, 2);
        for (const Outcome* out : {&bounded, &through}) {
            CHECK(out->fills.empty());
            CHECK(out->rejects.empty());
            CHECK(out->consulted.empty());
            CHECK(out->lots.empty());
            CHECK(out->position.lot_count == 0);
            CHECK(out->working == 1);
        }
        // The flag is folded whether or not it ever mattered: the working
        // request is broker state, so both identities differ over the same
        // empty book.
        CHECK(bounded.continuation != through.continuation);
        CHECK(bounded.broker != through.broker);
    }
}

// 4. Without slippage there is nothing to carry: both hosts book the level
//    and the same lot. The opt-in changes no price on its own.
void zero_slippage_books_the_level_on_both() {
    for (const auto& side : kSides) {
        scenario = side.name;
        const Outcome bounded = run_side(side, side.crossing, false, 0);
        const Outcome through = run_side(side, side.crossing, true, 0);
        one_fill(bounded, side, side.level, side.level, no::NativeCandidatePriceKind::TriggerLevel);
        one_fill(through, side, side.level, side.level, no::NativeCandidatePriceKind::TriggerLevel);
        CHECK(bounded.lots[0].entry_incarnation == through.lots[0].entry_incarnation);
        CHECK(bounded.lots[0].entry_time_ms == through.lots[0].entry_time_ms);
        CHECK(bounded.position.average_price == through.position.average_price);
        // Same book, different ledger: the filled request's definition still
        // carries the flag, and it is hashed like every durable request fact.
        CHECK(bounded.continuation != through.continuation);
        CHECK(bounded.broker != through.broker);
    }
}

// 5. The bound is re-checked after the host's own terms. A host that answers
//    a price past the level is refused with InvalidTerms on a bounded limit —
//    the request ends there, nothing fills — and is booked verbatim on a
//    fill-through one. The buy answers 99 against 98, the sell 101 against
//    102 (default + 1 and default - 1: the adverse direction of each side).
void host_terms_past_the_level() {
    for (const auto& side : kSides) {
        scenario = side.name;
        const double offset = side.units > 0.0 ? 1.0 : -1.0;
        const double answered = side.level + offset;
        const Outcome bounded = run_side(side, side.crossing, false, 0, offset);
        const Outcome through = run_side(side, side.crossing, true, 0, offset);

        CHECK(bounded.fills.empty());
        REQUIRE(bounded.rejects.size() == 1);
        CHECK(bounded.rejects[0].reason == no::MatchRejectReason::InvalidTerms);
        CHECK(bounded.rejects[0].request().label == "touch");
        REQUIRE(bounded.rejects[0].attempted_terms.has_value());
        CHECK(bounded.rejects[0].attempted_terms->resolved_price == answered);
        REQUIRE(bounded.consulted.size() == 1);
        CHECK(bounded.consulted[0].default_resolved_price == side.level);
        CHECK(bounded.lots.empty());
        CHECK(bounded.position.lot_count == 0);
        CHECK(bounded.working == 0);

        one_fill(through, side, side.level, answered, no::NativeCandidatePriceKind::TriggerLevel);
        CHECK(through.consulted[0].default_resolved_price == side.level);
    }
}

}  // namespace

int main() {
    test("crossing-is-capped-or-carried", crossing_is_capped_or_carried);
    test("gap-open-books-the-print", gap_open_books_the_print);
    test("untouched-level-gates-both", untouched_level_gates_both);
    test("zero-slippage-books-the-level-on-both", zero_slippage_books_the_level_on_both);
    test("host-terms-past-the-level", host_terms_past_the_level);
    std::printf("test_native_limit_fill_through: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
