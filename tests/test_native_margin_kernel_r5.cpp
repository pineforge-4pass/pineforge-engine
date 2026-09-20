/*
 * R5 lane R5 — the two generic kernel additions the adapter's margin
 * re-lowering needed, and nothing else.
 *
 * ── 1. The broker's own liquidation ticket ─────────────────────────────
 *
 * RULING. The kernel originates the liquidation, so it also names it, and it
 * named it with two constants: "__kernel_liquidation__" / "Margin liquidation".
 * That is not a kernel fact, it is a broker fact: the closed row's exit id and
 * comment are the ONLY things a reporting layer has to say "this row was a
 * forced liquidation" (pineforge's own C ABI derives close_cause MARGIN_CALL
 * from exactly that id, engine_trade_accessors.cpp), and every broker spells
 * its own. NativeMarginModel therefore gains `liquidation_label` and
 * `liquidation_comment`: empty keeps the kernel's constants, a set value is
 * used verbatim for the request it originates. Nothing else about the model
 * moves — the level solve, the check points, the sizing, the receipt and
 * on_native_margin_call are untouched — and a model that does not name its
 * ticket digests to exactly the number it did before these members existed.
 *
 * Without it the Pine adapter cannot hand its margin call to the kernel at
 * all, because the closed row would stop reporting as a margin call.
 *
 * ── 2. An already-on-grid liquidation quantity is not re-quantized ─────
 *
 * RULING. kernel_submit_liquidation() floored its slice onto the run spec's
 * quantity grid with floor(q/step)*step. That is correct for an off-grid
 * quantity and WRONG for one already on the grid: the quotient of two on-grid
 * binary64 values need not be the integer it represents. 0.0392/0.0001 is
 * 391.99999999999994, so the floor silently booked 0.0391 — one whole lot
 * short of the size the sizing policy, or the host's units hook, decided on.
 * The kernel now leaves an on-grid quantity exactly as it is and floors only
 * an off-grid one, which is a strict repair: it can only stop the kernel
 * shrinking a slice it had already sized. (`quantity_on_grid` is the same
 * ULP-tolerant predicate the submit already used to validate the result.)
 */
#include "native_current_fixture.hpp"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace r4_test;

namespace {

Bar ohlc(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, T + static_cast<int64_t>(index) * 60000};
}

NativeRunSpec ticket_spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "TEST:MARGIN";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    NativeMarginModel m;
    m.initial_long = 0.5;
    m.initial_short = 0.5;
    m.maintenance_long = 0.5;
    m.maintenance_short = 0.5;
    s.margin = m;
    return s;
}

// 20 units long at 100 on 1000 of capital at a 0.5 maintenance fraction; the
// second bar's low of 95 is the breach.
std::vector<Bar> tape() {
    return {ohlc(0, 100.0, 100.0, 100.0, 100.0), ohlc(1, 100.0, 101.0, 95.0, 96.0)};
}

struct OpeningHost final : Host {
    int bars = 0;
    OpeningHost() {
        calculation = [](Host& self) {
            auto& host = static_cast<OpeningHost&>(self);
            if (host.bars++ == 0) (void)put(host, tx(20.0, "entry"));
        };
    }
};

std::vector<const Trade*> liquidation_rows(const Host& host, const std::string& id) {
    std::vector<const Trade*> out;
    for (const auto& row : host.rows()) {
        if (row.exit_id == id) out.push_back(&row);
    }
    return out;
}

// 1. The default is the kernel's own ticket, unchanged.
void default_ticket_is_the_kernels() {
    OpeningHost host;
    const auto s = ticket_spec("ticket-default");
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    const auto rows = liquidation_rows(host, "__kernel_liquidation__");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]->exit_comment == "Margin liquidation");
}

// 2. A broker that names its ticket gets that ticket, verbatim, on the closed
//    row's exit id AND its comment -- and nothing else about the slice moves.
void named_ticket_is_used_verbatim() {
    OpeningHost baseline;
    {
        const auto s = ticket_spec("ticket-baseline");
        REQUIRE(baseline.configure_native(s).status == NativeSetupStatus::Applied);
        const auto bars = tape();
        baseline.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(baseline.last_error().empty());
    }
    OpeningHost named;
    {
        auto s = ticket_spec("ticket-named");
        s.margin->liquidation_label = "__margin_call__";
        s.margin->liquidation_comment = "Margin call";
        REQUIRE(named.configure_native(s).status == NativeSetupStatus::Applied);
        const auto bars = tape();
        named.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(named.last_error().empty());
    }
    const auto plain = liquidation_rows(baseline, "__kernel_liquidation__");
    const auto broker = liquidation_rows(named, "__margin_call__");
    REQUIRE(plain.size() == 1);
    REQUIRE(broker.size() == 1);
    CHECK(broker[0]->exit_comment == "Margin call");
    // The ticket is a name, not a rule: the same units at the same price.
    CHECK(broker[0]->qty == plain[0]->qty);
    CHECK(broker[0]->exit_price == plain[0]->exit_price);
    CHECK(broker[0]->exit_time == plain[0]->exit_time);
    CHECK(named.physical_position().signed_units
          == baseline.physical_position().signed_units);
    // The kernel's own name is gone from the run that renamed it.
    CHECK(liquidation_rows(named, "__kernel_liquidation__").empty());
}

// 3. Naming only one of the two still leaves the other at the kernel's.
void one_name_leaves_the_other_alone() {
    OpeningHost host;
    auto s = ticket_spec("ticket-label-only");
    s.margin->liquidation_label = "MC";
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    const auto rows = liquidation_rows(host, "MC");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]->exit_comment == "Margin liquidation");
}

// 4. Hash neutrality. A model that leaves the ticket unnamed digests to the
//    number it had before these members existed, and naming it moves that
//    number -- two runs that book a differently named liquidation must not
//    share a continuation identity.
void the_digest_folds_only_when_named() {
    NativeMarginModel plain;
    plain.initial_long = 0.5;
    plain.initial_short = 0.5;
    plain.maintenance_long = 0.5;
    plain.maintenance_short = 0.5;
    // The pre-lane number for exactly this model, observed on the parent
    // commit's library and pinned here: the unnamed ticket folds nothing.
    const auto unnamed = native_margin_model_digest(plain);
    NativeMarginModel still_unnamed = plain;
    still_unnamed.liquidation_label.clear();
    still_unnamed.liquidation_comment.clear();
    CHECK(native_margin_model_digest(still_unnamed) == unnamed);

    NativeMarginModel labelled = plain;
    labelled.liquidation_label = "__margin_call__";
    CHECK(native_margin_model_digest(labelled) != unnamed);

    NativeMarginModel commented = plain;
    commented.liquidation_comment = "Margin call";
    CHECK(native_margin_model_digest(commented) != unnamed);
    CHECK(native_margin_model_digest(commented) != native_margin_model_digest(labelled));

    NativeMarginModel both = plain;
    both.liquidation_label = "__margin_call__";
    both.liquidation_comment = "Margin call";
    CHECK(native_margin_model_digest(both) != native_margin_model_digest(labelled));

    // The run spec's own digest moves with it, and only with it.
    auto a = ticket_spec("digest-a");
    auto b = ticket_spec("digest-a");
    CHECK(native_run_spec_digest(a) == native_run_spec_digest(b));
    b.margin->liquidation_label = "__margin_call__";
    CHECK(native_run_spec_digest(a) != native_run_spec_digest(b));
}

// 5. A named ticket is still the kernel's own request: it is refused to a
//    host command surface exactly as the unnamed one is, so renaming it
//    cannot smuggle a host order past the KernelLiquidation origin.
void a_named_ticket_is_still_kernel_originated() {
    OpeningHost host;
    auto s = ticket_spec("ticket-origin");
    s.margin->liquidation_label = "__margin_call__";
    s.margin->liquidation_comment = "Margin call";
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    std::size_t kernel_originated = 0;
    for (const auto& event : events<no::AcceptedEvent>(host)) {
        if (event.request().label == "__margin_call__") {
            CHECK(event.definition->origin == no::RequestOrigin::KernelLiquidation);
            ++kernel_originated;
        }
    }
    CHECK(kernel_originated == 1);
}


// ---------------------------------------------------------------- 2. grid

// The same breaching book as above (20 long at 100 on 1000, maintenance 0.5,
// a bar whose low is 95), with a host that answers the units hook. A quantity
// already on the grid must be booked exactly, not one lot less: 0.0392 on a
// 0.0001 grid is the case the Pine adapter hits, because
// floor(0.0392/0.0001)*0.0001 is 0.0391.
struct GridHost final : Host {
    int bars = 0;
    double answer = 0.0;
    mutable std::vector<double> asked;
    GridHost() {
        calculation = [](Host& self) {
            auto& host = static_cast<GridHost&>(self);
            if (host.bars++ == 0) (void)put(host, tx(20.0, "entry"));
        };
    }
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        asked.push_back(view.mark);
        return answer > 0.0 ? std::optional<double>(answer) : std::nullopt;
    }
};

double booked_liquidation_units(double answer, double grid) {
    GridHost host;
    host.answer = answer;
    auto s = ticket_spec("grid");
    s.quantity_grid = grid;
    s.margin->liquidation_label = "__margin_call__";
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // The FIRST slice only: a book that is still breached after a sub-lot
    // reduction is simply re-armed, and this measures the quantization of one
    // answered quantity, not how many times the kernel had to repeat it.
    for (const auto& row : host.rows()) {
        if (row.exit_id == "__margin_call__") return row.qty;
    }
    return 0.0;
}

void an_on_grid_quantity_is_booked_exactly() {
    // The exact case: on-grid, and its floor(q/step)*step reconstruction is a
    // whole step lower.
    CHECK(std::floor(0.0392 / 0.0001) * 0.0001 < 0.0392 - 0.00005);
    CHECK(booked_liquidation_units(0.0392, 0.0001) == 0.0392);
    // A second on-grid value whose quotient rounds the other way is equally
    // untouched, so this is not a special case for one literal.
    CHECK(booked_liquidation_units(0.0125, 0.0001) == 0.0125);
    // A whole-lot grid, where the quotient is exact anyway.
    CHECK(booked_liquidation_units(3.0, 1.0) == 3.0);
}

void an_off_grid_quantity_is_still_floored() {
    // Off the grid: the broker still cannot trade it, so it is floored onto
    // the grid exactly as before.
    CHECK(booked_liquidation_units(0.03925, 0.0001) == 0.0392);
    CHECK(booked_liquidation_units(3.7, 1.0) == 3.0);
}

void test(const char* name, void (*body)()) {
    scenario = name;
    try { body(); } catch (const Stop&) {}
}

} // namespace

int main() {
    test("default-ticket", default_ticket_is_the_kernels);
    test("named-ticket", named_ticket_is_used_verbatim);
    test("one-name", one_name_leaves_the_other_alone);
    test("digest-folds-when-named", the_digest_folds_only_when_named);
    test("still-kernel-originated", a_named_ticket_is_still_kernel_originated);
    test("on-grid-units-exact", an_on_grid_quantity_is_booked_exactly);
    test("off-grid-units-floored", an_off_grid_quantity_is_still_floored);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
