// R5 L3: kernel sizing bases and fractional reduces on a bare native host.
// Every scenario drives NativeStrategyHost only; nothing here includes a
// source or compat header, and the last case proves the Pine adapter never
// names the new value kinds.
#include "native_terms_fixture.hpp"

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using namespace r4_test;
using namespace r4_terms;

static_assert(std::variant_size_v<no::OrderIntent> == 6);
static_assert(std::is_same_v<std::variant_alternative_t<5, no::OrderIntent>, no::Sized>);
static_assert(std::variant_size_v<no::ReductionSize> == 3);
static_assert(std::is_same_v<std::variant_alternative_t<2, no::ReductionSize>, no::ScopeFraction>);
static_assert(std::variant_size_v<no::SizeBasis> == 2);
static_assert(std::is_same_v<std::variant_alternative_t<0, no::SizeBasis>, no::CashValue>);

namespace {

// --- request builders ------------------------------------------------------

no::Request sized_open(no::SizeBasis basis,
                       no::Side side = no::Side::Long,
                       no::SizeTime time = no::SizeTime::AtMatch,
                       bool reserve_fee = false,
                       const char* label = "kernel-open") {
    no::Sized sized;
    sized.side = side;
    sized.basis = basis;
    sized.time = time;
    sized.reserve_percent_fee = reserve_fee;
    no::Request out;
    out.intent = sized;
    out.label = label;
    return out;
}

no::Request fraction_close(double fraction,
                           no::ScopeClaim claim = no::ScopeClaim::Gross,
                           const char* label = "fraction-close") {
    no::Request out;
    out.intent = no::Reduce{no::ScopeFraction{fraction, claim}};
    out.label = label;
    return out;
}

// --- the hand computation the differential control uses --------------------

// Mirrors the engine's quantity step: a floor that keeps an already-on-grid
// binary64 value as it stands.
double hand_floor(double units, double step) {
    if (!(units > 0.0) || !(step > 0.0)) return units;
    if (no::quantity_on_grid(units, step)) return units;
    const double floored = std::floor(units / step + 1e-6) * step;
    return floored < units ? floored : units;
}

double hand_units(double cash, double price, double point_value, double fx) {
    return cash / (price * point_value * fx);
}

// --- shared assertions -----------------------------------------------------

void same_applied(const no::ExecutionAppliedEvent& a, const no::ExecutionAppliedEvent& b) {
    CHECK(a.ordinal == b.ordinal);
    CHECK(bits(a.raw_price) == bits(b.raw_price));
    CHECK(bits(a.resolved_price) == bits(b.resolved_price));
    CHECK(bits(a.current_ticket) == bits(b.current_ticket));
    CHECK(bits(a.closed_units) == bits(b.closed_units));
    CHECK(bits(a.opened_units) == bits(b.opened_units));
    CHECK(bits(a.filled_working) == bits(b.filled_working));
    CHECK(a.opened_lot_incarnation == b.opened_lot_incarnation);
    CHECK(a.cycle_after == b.cycle_after);
    CHECK(a.terminal == b.terminal);
}

void same_book(const TermsHost& a, const TermsHost& b) {
    REQUIRE(a.lots().size() == b.lots().size());
    for (std::size_t i = 0; i < a.lots().size(); ++i) {
        CHECK(bits(a.lots()[i].qty) == bits(b.lots()[i].qty));
        CHECK(bits(a.lots()[i].price) == bits(b.lots()[i].price));
        CHECK(bits(a.lots()[i].entry_commission_account)
              == bits(b.lots()[i].entry_commission_account));
    }
    CHECK(bits(a.net()) == bits(b.net()));
}

// One opening per run, executed through the guarded current-execution target
// on the first calculation.
no::ExecutionAppliedEvent open_once(TermsHost& host, const NativeRunSpec& setup,
                                    const no::Request& request,
                                    std::initializer_list<double> prices) {
    std::optional<no::ExecutionAppliedEvent> applied;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (applied) return;
        applied = apply(h, put(h, request));
    };
    run(host, setup, prices);
    completed(host);
    REQUIRE(applied);
    return *applied;
}

// --- 1. differential: Sized vs a host-resolved HostSized on the same facts --

void differential_sized_matches_hand_resolved_host_sized() {
    struct Case {
        const char* name;
        double price;
        NativeFeeKind fee_kind;
        double fee_value;
        bool reserve_fee;
        std::optional<double> grid;
    };
    const Case cases[] = {
        {"zero fee, no grid", 100.0, NativeFeeKind::CashPerExecution, 0.0, false, std::nullopt},
        {"percent fee reserve", 100.0, NativeFeeKind::Percent, 0.001, true, std::nullopt},
        {"quantity grid", 300.0, NativeFeeKind::CashPerExecution, 0.0, false, 1.0},
    };
    for (const auto& row : cases) {
        auto setup = spec("l3-differential");
        setup.fee_kind = row.fee_kind;
        setup.fee_value = row.fee_value;
        setup.quantity_grid = row.grid;

        TermsHost kernel;
        const auto kernel_applied = open_once(
            kernel, setup,
            sized_open(no::EquityFraction{1.0}, no::Side::Long, no::SizeTime::AtMatch,
                       row.reserve_fee),
            {row.price});

        TermsHost hand;
        const bool reserve = row.reserve_fee && row.fee_kind == NativeFeeKind::Percent;
        const double fee_value = row.fee_value;
        const auto grid = row.grid;
        hand.resolver = [&hand, reserve, fee_value, grid](
                const NativeExecutionTermsFacts& facts) {
            const double price = facts.default_resolved_price;
            double cash = hand.native_marked_equity(price);
            if (reserve) cash /= 1.0 + fee_value;
            double units = hand_units(cash, price, 1.0, facts.active_fx);
            if (grid) units = hand_floor(units, *grid);
            return no::ExecutionTerms{price, units, no::OpeningShape::Transact};
        };
        const auto hand_applied = open_once(
            hand, setup, host_open(no::Side::Long, "kernel-open"), {row.price});

        scenario = row.name;
        same_applied(kernel_applied, hand_applied);
        same_book(kernel, hand);
        CHECK(kernel_applied.opened_units > 0.0);
        if (row.grid) CHECK(no::quantity_on_grid(kernel_applied.opened_units, *row.grid));
        // The host saw the kernel resolution before it answered.
        REQUIRE(!hand.resolved_facts.empty());
        CHECK(std::holds_alternative<no::RemainingDeferred>(hand.resolved_facts.back().remaining));
        REQUIRE(!kernel.resolved_facts.empty());
        const auto* published = std::get_if<no::RemainingUnits>(
            &kernel.resolved_facts.back().remaining);
        REQUIRE(published);
        CHECK(bits(published->q) == bits(kernel_applied.opened_units));
    }
}

// --- 2. cash value, and the acceptance/match sizing point ------------------

void cash_value_units_are_exact_and_the_sizing_point_matters() {
    for (const double price : {8.0, 300.0}) {
        TermsHost host;
        const auto applied = open_once(host, spec("l3-cash"),
                                       sized_open(no::CashValue{1000.0}), {price});
        scenario = "cash value exactness";
        CHECK(bits(applied.opened_units) == bits(1000.0 / price));
        CHECK(bits(applied.resolved_price) == bits(price));
    }

    // A market request accepted on bar 0 rests to bar 1's open. AtAcceptance
    // freezes bar 0's close; AtMatch resolves against the bar 1 open.
    auto setup = spec("l3-sizing-point");
    setup.close_execution = NativeCloseExecution::NextEligiblePoint;
    TermsHost host;
    std::vector<double> opened;
    host.notification = [&](Host&, const no::ExecutionAppliedEvent& event) {
        opened.push_back(event.opened_units);
    };
    bool submitted = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (submitted) return;
        submitted = true;
        put(h, sized_open(no::CashValue{1000.0}, no::Side::Long,
                          no::SizeTime::AtAcceptance, false, "frozen"));
        put(h, sized_open(no::CashValue{1000.0}, no::Side::Long,
                          no::SizeTime::AtMatch, false, "at-match"));
    };
    run(host, setup, {100.0, 200.0, 200.0});
    completed(host);
    scenario = "acceptance vs match sizing point";
    REQUIRE(opened.size() == 2);
    CHECK(bits(opened[0]) == bits(1000.0 / 100.0));
    CHECK(bits(opened[1]) == bits(1000.0 / 200.0));
}

// --- 3. rejections ---------------------------------------------------------

void invalid_bases_are_rejected_and_dust_is_unresolved() {
    TermsHost host;
    bool reached = false;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (reached) return;
        reached = true;
        const no::Request rejected[] = {
            sized_open(no::EquityFraction{0.0}),
            sized_open(no::EquityFraction{1.5}),
            sized_open(no::EquityFraction{std::numeric_limits<double>::quiet_NaN()}),
            sized_open(no::CashValue{0.0}),
            sized_open(no::CashValue{-10.0}),
            fraction_close(0.0),
            fraction_close(1.5),
        };
        for (const auto& request : rejected) {
            const auto result = h.submit(request);
            CHECK(result.status == no::SubmitStatus::Rejected);
            REQUIRE(result.reason.has_value());
            CHECK(*result.reason == no::RequestRejectReason::InvalidQuantityBasis);
            CHECK(!result.handle.has_value());
        }
        // The boundary values stay accepted.
        CHECK(h.submit(sized_open(no::EquityFraction{1.0})).status
              == no::SubmitStatus::Accepted);
        CHECK(h.submit(fraction_close(1.0)).status == no::SubmitStatus::Accepted);
    };
    run(host, spec("l3-reject"), {100.0});
    completed(host);
    CHECK(reached);

    // Below one grid step after snapping: the basis is not representable and
    // the candidate reports TermsUnresolved rather than a zero-unit fill.
    auto setup = spec("l3-dust");
    setup.quantity_grid = 1.0;
    TermsHost dust;
    bool dust_reached = false;
    dust.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (dust_reached) return;
        dust_reached = true;
        const auto target = put(h, sized_open(no::CashValue{5.0}));
        const auto outcome = h.execute_current(command(target));
        const auto* rejected = std::get_if<no::MatchRejectedEvent>(&outcome);
        REQUIRE(rejected);
        CHECK(rejected->reason == no::MatchRejectReason::TermsUnresolved);
        CHECK(h.lots().empty());
    };
    run(dust, setup, {100.0});
    completed(dust);
    CHECK(dust_reached);
}

// ExplicitUnits keeps the literal quotient. On a gridded run that is admitted
// only when the quotient already sits on the grid.
void explicit_units_policy_keeps_the_literal_quotient() {
    auto setup = spec("l3-explicit-grid");
    setup.quantity_grid = 1.0;
    for (const double price : {250.0, 300.0}) {
        const bool on_grid = price == 250.0;   // 1000 / 250 == 4, 1000 / 300 is not
        TermsHost host;
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            auto request = sized_open(no::CashValue{1000.0});
            std::get<no::Sized>(request.intent).grid_policy = no::ExecutionGridPolicy::ExplicitUnits;
            const auto outcome = h.execute_current(command(put(h, request)));
            if (on_grid) {
                const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&outcome);
                REQUIRE(applied);
                CHECK(bits(applied->opened_units) == bits(1000.0 / price));
            } else {
                const auto* rejected = std::get_if<no::MatchRejectedEvent>(&outcome);
                REQUIRE(rejected);
                CHECK(rejected->reason == no::MatchRejectReason::InvalidTerms);
                CHECK(h.lots().empty());
            }
        };
        scenario = on_grid ? "explicit units on grid" : "explicit units off grid";
        run(host, setup, {price});
        completed(host);
        CHECK(reached);
    }
}

// --- 4. scope fractions ----------------------------------------------------

no::Request resting_reduce(double units, double stop_price, const char* label) {
    no::Request out = reduce(units, label);
    out.trigger = no::Stop{stop_price};
    return out;
}

void scope_fractions_claim_gross_or_net_of_siblings() {
    struct Row {
        const char* name;
        no::ScopeClaim claim;
        double expected;
    };
    // One 10-unit lot with a live 5-unit sibling reduce bound to the same
    // book: Gross claims 5 (5 + 5 across the two siblings), NetOfSiblings
    // claims half of the unclaimed 5.
    for (const Row& row : {Row{"gross sibling claim", no::ScopeClaim::Gross, 5.0},
                           Row{"net-of-siblings claim", no::ScopeClaim::NetOfSiblings, 2.5}}) {
        TermsHost host;
        bool reached = false;
        host.calculation = [&](Host& base) {
            auto& h = static_cast<TermsHost&>(base);
            if (reached) return;
            reached = true;
            (void)apply(h, put(h, tx(10.0, "lot")));
            REQUIRE(h.lots().size() == 1);
            const auto sibling = put(h, resting_reduce(5.0, 1.0, "sibling"));
            (void)sibling;
            const auto applied = apply(h, put(h, fraction_close(0.5, row.claim)));
            CHECK(bits(applied.closed_units) == bits(row.expected));
            CHECK(bits(h.lots().front().qty) == bits(10.0 - row.expected));
        };
        scenario = row.name;
        run(host, spec("l3-scope"), {100.0});
        completed(host);
        CHECK(reached);
    }

    // A partial close, then a further fraction of what is left.
    TermsHost sequential;
    bool sequential_reached = false;
    sequential.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        if (sequential_reached) return;
        sequential_reached = true;
        (void)apply(h, put(h, tx(10.0, "lot")));
        const auto first = apply(h, put(h, fraction_close(0.5, no::ScopeClaim::Gross, "half")));
        CHECK(bits(first.closed_units) == bits(5.0));
        const auto second = apply(h, put(h, fraction_close(0.5, no::ScopeClaim::Gross, "half-again")));
        CHECK(bits(second.closed_units) == bits(2.5));
        CHECK(bits(h.lots().front().qty) == bits(2.5));
    };
    scenario = "partial close then a further fraction";
    run(sequential, spec("l3-sequential"), {100.0});
    completed(sequential);
    CHECK(sequential_reached);
}

void a_pending_parent_defers_the_fraction_until_the_parent_fills() {
    auto setup = spec("l3-bracket");
    setup.close_execution = NativeCloseExecution::NextEligiblePoint;
    TermsHost host;
    std::vector<double> closed;
    int calculations = 0;
    std::optional<no::RequestHandle> child;
    host.notification = [&](Host&, const no::ExecutionAppliedEvent& event) {
        if (event.request().label == "bracket-child") closed.push_back(event.closed_units);
    };
    host.calculation = [&](Host& base) {
        auto& h = static_cast<TermsHost&>(base);
        ++calculations;
        if (calculations == 1) {
            const auto parent = put(h, tx(10.0, "bracket-parent"));
            auto request = fraction_close(0.5, no::ScopeClaim::Gross, "bracket-child");
            request.owner = no::WaitForApplied{parent};
            child = put(h, request);
            // Before the parent fills the child has no scope at all.
            const auto outcome = h.execute_current(command(*child));
            const auto* refusal = std::get_if<NativeCurrentRefusal>(&outcome);
            REQUIRE(refusal);
            CHECK(*refusal == NativeCurrentRefusal::UnreadyOwner);
            CHECK(closed.empty());
        }
    };
    run(host, setup, {100.0, 100.0, 100.0});
    completed(host);
    scenario = "bracket child resolves after the parent fill";
    REQUIRE(closed.size() == 1);
    CHECK(bits(closed.front()) == bits(5.0));
    CHECK(host.lots().size() == 1);
    CHECK(bits(host.lots().front().qty) == bits(5.0));
}

// --- 5. adapter neutrality --------------------------------------------------

bool names_identifier(const std::string& text, const std::string& name) {
    for (std::size_t at = text.find(name); at != std::string::npos;
         at = text.find(name, at + 1)) {
        const auto word = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_';
        };
        const bool left = at > 0 && word(text[at - 1]);
        const std::size_t after = at + name.size();
        const bool right = after < text.size() && word(text[after]);
        if (!left && !right) return true;
    }
    return false;
}

void the_source_layer_never_names_the_kernel_bases() {
#ifdef PINEFORGE_SOURCE_LAYER_FILES
    std::vector<std::string> paths;
    {
        std::string joined = PINEFORGE_SOURCE_LAYER_FILES;
        std::size_t start = 0;
        while (start <= joined.size()) {
            const std::size_t at = joined.find('|', start);
            const std::string piece = joined.substr(
                start, at == std::string::npos ? std::string::npos : at - start);
            if (!piece.empty()) paths.push_back(piece);
            if (at == std::string::npos) break;
            start = at + 1;
        }
    }
    // A vacuous pass would be worse than a failure.
    REQUIRE(paths.size() >= 4);
    const char* names[] = {"Sized", "ScopeFraction", "CashValue", "EquityFraction",
                           "SizeBasis", "SizeTime", "ScopeClaim"};
    for (const auto& path : paths) {
        std::ifstream input(path);
        REQUIRE(input.good());
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string text = buffer.str();
        CHECK(!text.empty());
        for (const char* name : names) {
            const bool found = names_identifier(text, name);
            if (found) std::printf("  adapter names %s in %s\n", name, path.c_str());
            CHECK(!found);
        }
    }
#else
    std::printf("  PINEFORGE_SOURCE_LAYER_FILES is undefined\n");
    CHECK(false);
#endif
}

}  // namespace

int main() {
    test("differential Sized vs hand-resolved HostSized",
         differential_sized_matches_hand_resolved_host_sized);
    test("cash value and sizing point", cash_value_units_are_exact_and_the_sizing_point_matters);
    test("invalid bases and unrepresentable dust",
         invalid_bases_are_rejected_and_dust_is_unresolved);
    test("explicit units grid policy", explicit_units_policy_keeps_the_literal_quotient);
    test("scope fraction gross and net of siblings",
         scope_fractions_claim_gross_or_net_of_siblings);
    test("pending bracket parent", a_pending_parent_defers_the_fraction_until_the_parent_fills);
    test("adapter neutrality", the_source_layer_never_names_the_kernel_bases);
    std::printf("R5 L3 sizing bases: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
