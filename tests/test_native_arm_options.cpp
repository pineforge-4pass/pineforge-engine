// R5 gap lane N13: the two arm options of native_order::WaitForApplied.
//
// A request that waits for its owner is armed inside the owner's fill
// settlement. Two generic, opt-in facts about that arm were missing, and both
// are what separates "a bracket child placed WITH its entry" from "the same
// close submitted from the entry's fill callback":
//
//   NativeArmFirstMatch  AtArmPrint (default): the armed child is a candidate
//                        at the owner's fill print. AfterArmPrint: it has the
//                        birth rule of a callback-born request -- the print
//                        that armed it is consumed, a later crossing on that
//                        driver point still matches, the next point is
//                        ordinary.
//   NativeArmScope       OwnerLot (default): an armed closing child closes the
//                        lot its owner opened. Book: it is bound, at the arm,
//                        to the whole position that fill left -- the authority
//                        an Independent close acquires -- and only then may
//                        it be a HostSized close.
//
// Each option is pinned two ways: against the default (the observable
// difference) and against its TWIN, a host that submits the equivalent
// request from on_native_applied (the semantic definition). The defaults are
// pinned by the unchanged suites; here a default-spelled child is also
// required to keep its continuation identity, and a non-default one to move
// it (the conditional hash folds).
//
// Fail-before: compiled against the previous header closure (engine main
// 683a82f) this unit stops at `error: no member named 'NativeArmFirstMatch' in
// namespace 'pineforge::native_order'`.
#include "native_current_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

using namespace r4_test;

namespace {

constexpr std::int64_t kT = 1700000040000LL;

Bar ohlc(int index, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, kT + static_cast<std::int64_t>(index) * 60000};
}

NativeRunSpec arm_spec(const char* key) {
    NativeRunSpec s = spec(key);
    s.close_execution = NativeCloseExecution::NextEligiblePoint;
    return s;
}

struct Row {
    std::int64_t entry_time, exit_time;
    double entry_price, exit_price, qty;
    int open_at_end;
};

bool same_rows(const std::vector<Row>& a, const std::vector<Row>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].entry_time != b[i].entry_time || a[i].exit_time != b[i].exit_time
            || a[i].entry_price != b[i].entry_price || a[i].exit_price != b[i].exit_price
            || a[i].qty != b[i].qty || a[i].open_at_end != b[i].open_at_end) {
            return false;
        }
    }
    return true;
}

struct Outcome {
    std::vector<Row> rows;
    double position = 0.0;
    std::uint64_t continuation = 0;
    bool ok = false;
};

Outcome finish(Host& host) {
    Outcome out;
    out.ok = host.last_error().empty()
        && host.native_state().kind == NativeLifecycleKind::Completed;
    if (!host.last_error().empty()) std::printf("  run error: %s\n", host.last_error().c_str());
    ReportC report{};
    host.fill_report(&report);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        out.rows.push_back({t.entry_time, t.exit_time, t.entry_price, t.exit_price, t.qty,
                            t.open_at_end});
    }
    BacktestEngine::free_report(&report);
    out.position = host.physical_position().signed_units;
    out.continuation = host.native_continuation_hash();
    return out;
}

void print_rows(const char* label, const Outcome& out) {
    std::printf("  %-22s position=%g rows=%zu\n", label, out.position, out.rows.size());
    for (const auto& r : out.rows) {
        std::printf("    entry %.17g @%lld  exit %.17g @%lld  qty %.17g open=%d\n", r.entry_price,
                    static_cast<long long>((r.entry_time - kT) / 60000), r.exit_price,
                    static_cast<long long>((r.exit_time - kT) / 60000), r.qty, r.open_at_end);
    }
}

// ── NativeArmFirstMatch ─────────────────────────────────────────────────
// A buy-stop parent crossed INSIDE a rising segment, and a take-profit sell
// limit anchored half a point BELOW the fill: the fill print already
// satisfies it. At the arm print it books that print; after it, the rising
// segment never re-crosses the level, so the first match is the next driver
// point.
std::vector<Bar> first_match_feed() {
    return {
        ohlc(0, 100.0, 100.0, 100.0, 100.0),   // parent + child placed
        ohlc(1, 100.0, 103.0, 100.0, 103.0),   // 101 crossed mid-segment
        ohlc(2, 103.0, 104.0, 102.0, 104.0),
        ohlc(3, 104.0, 104.0, 104.0, 104.0),
    };
}

enum class FirstMatchHost { AtArmPrint, AfterArmPrint, CallbackTwin };

Outcome run_first_match(FirstMatchHost kind, const char* key) {
    Host host;
    no::RequestHandle parent;
    bool placed = false;
    const auto child = [&](no::Owner owner, double level, no::TriggerAnchor anchor) {
        no::Request leg{no::Reduce{no::ExplicitUnits{1.0}}, "tp", ""};
        leg.trigger = no::Limit{level};
        leg.owner = std::move(owner);
        leg.anchor = anchor;
        return leg;
    };
    host.calculation = [&](Host& base) {
        if (placed) return;
        placed = true;
        no::Request entry = tx(1.0, "entry");
        entry.trigger = no::Stop{101.0};
        parent = put(base, entry);
        if (kind == FirstMatchHost::CallbackTwin) return;
        put(base, child(no::WaitForApplied{
                            parent, no::NativeArmVisibility::Working,
                            kind == FirstMatchHost::AfterArmPrint
                                ? no::NativeArmFirstMatch::AfterArmPrint
                                : no::NativeArmFirstMatch::AtArmPrint},
                        0.0, no::FromOwnerFill{-0.5}));
    };
    host.notification = [&](Host& base, const no::ExecutionAppliedEvent& event) {
        if (kind != FirstMatchHost::CallbackTwin || event.handle() != parent) return;
        // The twin: the same limit, born in the owner's fill callback.
        put(base, child(no::Independent{}, event.resolved_price - 0.5, no::Absolute{}));
    };
    REQUIRE(host.configure_native(arm_spec(key)).status == NativeSetupStatus::Applied);
    const auto bars = first_match_feed();
    host.run(bars.data(), static_cast<int>(bars.size()));
    return finish(host);
}

void first_match_after_the_arm_print() {
    const Outcome at = run_first_match(FirstMatchHost::AtArmPrint, "n13-first-at");
    const Outcome after = run_first_match(FirstMatchHost::AfterArmPrint, "n13-first-after");
    const Outcome twin = run_first_match(FirstMatchHost::CallbackTwin, "n13-first-twin");
    print_rows("AtArmPrint", at);
    print_rows("AfterArmPrint", after);
    print_rows("callback twin", twin);
    CHECK(at.ok); CHECK(after.ok); CHECK(twin.ok);
    REQUIRE(at.rows.size() == 1 && after.rows.size() == 1 && twin.rows.size() == 1);
    // The established default: the armed child books the very print that
    // filled its owner, inside bar 1.
    CHECK(at.rows[0].entry_price == 101.0);
    CHECK(at.rows[0].exit_price == 101.0);
    CHECK(at.rows[0].exit_time == at.rows[0].entry_time);
    // AfterArmPrint: that print is consumed and the rising segment never
    // crosses 100.5 again, so the child first matches at the next driver
    // point, above the arm print.
    CHECK(after.rows[0].entry_price == 101.0);
    CHECK(after.rows[0].exit_price > 101.0);
    CHECK(!same_rows(at.rows, after.rows));
    // ...which is exactly the request born in the owner's fill callback.
    CHECK(same_rows(after.rows, twin.rows));
    CHECK(after.position == 0.0 && twin.position == 0.0 && at.position == 0.0);
}

// A sell stop anchored ABOVE the fill is inside its region at the arm print
// too; a discrete (bar-open) owner fill leaves no later crossing on the arming
// point at all, and the next point is ordinary.
void first_match_at_a_discrete_print() {
    const auto run = [&](no::NativeArmFirstMatch rule, const char* key) {
        Host host;
        bool placed = false;
        host.calculation = [&](Host& base) {
            if (placed) return;
            placed = true;
            const auto parent = put(base, tx(1.0, "entry"));   // fills at bar 1's open
            no::Request leg{no::Reduce{no::OwnerOpenedUnits{}}, "sl", ""};
            leg.trigger = no::Stop{0.0};
            leg.owner = no::WaitForApplied{parent, no::NativeArmVisibility::Working, rule};
            leg.anchor = no::FromOwnerFill{0.25};
            put(base, leg);
        };
        REQUIRE(host.configure_native(arm_spec(key)).status == NativeSetupStatus::Applied);
        const std::vector<Bar> bars = {
            ohlc(0, 100.0, 100.0, 100.0, 100.0),
            ohlc(1, 100.0, 102.0, 100.0, 102.0),
            ohlc(2, 102.0, 102.0, 102.0, 102.0),
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        return finish(host);
    };
    const Outcome at = run(no::NativeArmFirstMatch::AtArmPrint, "n13-discrete-at");
    const Outcome after = run(no::NativeArmFirstMatch::AfterArmPrint, "n13-discrete-after");
    print_rows("discrete AtArmPrint", at);
    print_rows("discrete AfterArmPrint", after);
    CHECK(at.ok); CHECK(after.ok);
    REQUIRE(at.rows.size() == 1 && after.rows.size() == 1);
    // Both stop out at the 100 open print (the stop at 100.25 is already
    // reached); the rule moves WHEN, never whether, and never past the point
    // that follows the arm.
    CHECK(at.rows[0].exit_price == 100.0);
    CHECK(after.rows[0].exit_price == 100.0);
    CHECK(at.rows[0].exit_time == after.rows[0].exit_time);
    CHECK(at.position == 0.0 && after.position == 0.0);
}

// ── NativeArmScope ──────────────────────────────────────────────────────
// A protective stop placed with its entry, and a later add. OwnerLot closes
// the lot the entry opened and leaves the add; Book closes the position.
struct BookHost final : Host {
    native_order::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        native_order::ExecutionTerms terms = Host::resolve_execution_terms(facts);
        const auto* sized = std::get_if<no::HostSized>(&facts.definition->request.intent);
        if (sized && sized->kind == no::HostSizedKind::Close) {
            terms.units = facts.scope_exposure_units;      // the whole scope
        }
        return terms;
    }
};

std::vector<Bar> scope_feed() {
    return {
        ohlc(0, 100.0, 100.0, 100.0, 100.0),   // entry + stop placed
        ohlc(1, 100.0, 101.0, 100.0, 101.0),   // entry fills @100, stop arms @98
        ohlc(2, 101.0, 101.0, 101.0, 101.0),   // add placed
        ohlc(3, 101.0, 101.0, 100.5, 100.5),   // add fills @101
        ohlc(4, 100.5, 100.5,  97.0,  97.0),   // through 98
        ohlc(5,  97.0,  97.0,  97.0,  97.0),
    };
}

enum class ScopeHost { OwnerLot, Book, CallbackTwin };

Outcome run_scope(ScopeHost kind, const char* key) {
    BookHost host;
    no::RequestHandle parent;
    int calculation = 0;
    const auto stop_leg = [&](no::Owner owner, double level, no::TriggerAnchor anchor,
                              bool host_sized) {
        no::Request leg{host_sized
                            ? no::OrderIntent{no::HostSized{no::HostSizedKind::Close,
                                                            std::nullopt}}
                            : no::OrderIntent{no::Reduce{no::OwnerOpenedUnits{}}},
                        "sl", ""};
        leg.trigger = no::Stop{level};
        leg.owner = std::move(owner);
        leg.anchor = anchor;
        return leg;
    };
    host.calculation = [&](Host& base) {
        const int bar = calculation++;
        if (bar == 0) {
            parent = put(base, tx(1.0, "entry"));
            if (kind == ScopeHost::OwnerLot) {
                put(base, stop_leg(no::WaitForApplied{parent}, 0.0, no::FromOwnerFill{-2.0},
                                   false));
            } else if (kind == ScopeHost::Book) {
                put(base, stop_leg(no::WaitForApplied{parent, no::NativeArmVisibility::Working,
                                                      no::NativeArmFirstMatch::AtArmPrint,
                                                      no::NativeArmScope::Book},
                                   0.0, no::FromOwnerFill{-2.0}, true));
            }
        }
        if (bar == 2) put(base, tx(1.0, "add"));
    };
    host.notification = [&](Host& base, const no::ExecutionAppliedEvent& event) {
        if (kind != ScopeHost::CallbackTwin || event.handle() != parent) return;
        put(base, stop_leg(no::Independent{}, event.resolved_price - 2.0, no::Absolute{}, true));
    };
    REQUIRE(host.configure_native(arm_spec(key)).status == NativeSetupStatus::Applied);
    const auto bars = scope_feed();
    host.run(bars.data(), static_cast<int>(bars.size()));
    return finish(host);
}

void book_scope_covers_a_later_add() {
    const Outcome lot = run_scope(ScopeHost::OwnerLot, "n13-scope-lot");
    const Outcome book = run_scope(ScopeHost::Book, "n13-scope-book");
    const Outcome twin = run_scope(ScopeHost::CallbackTwin, "n13-scope-twin");
    print_rows("OwnerLot", lot);
    print_rows("Book", book);
    print_rows("callback twin", twin);
    CHECK(lot.ok); CHECK(book.ok); CHECK(twin.ok);
    // The established default: only the lot the owner opened leaves at 98.
    CHECK(lot.position == 1.0);
    // Book: the armed close is the position's; both lots leave at 98.
    CHECK(book.position == 0.0);
    REQUIRE(book.rows.size() == 2);
    CHECK(book.rows[0].exit_price == 98.0 && book.rows[1].exit_price == 98.0);
    CHECK(book.rows[0].open_at_end == 0 && book.rows[1].open_at_end == 0);
    // ...exactly what an Independent host-sized close born in the owner's
    // fill callback does.
    CHECK(same_rows(book.rows, twin.rows));
    CHECK(twin.position == 0.0);
}

// A Book child is armed INTO the book authority: its ArmedEvent carries a
// BookClose for the cycle and side the owner's fill left, bound at that
// fill's cursor.
void book_scope_arms_into_the_book_authority() {
    BookHost host;
    no::RequestHandle parent, leg_handle;
    bool placed = false;
    host.calculation = [&](Host& base) {
        if (placed) return;
        placed = true;
        parent = put(base, tx(-2.0, "short-entry"));
        no::Request leg{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "sl", ""};
        leg.trigger = no::Stop{0.0};
        leg.owner = no::WaitForApplied{parent, no::NativeArmVisibility::PendingUntilArmed,
                                       no::NativeArmFirstMatch::AfterArmPrint,
                                       no::NativeArmScope::Book};
        leg.anchor = no::FromOwnerFill{3.0};
        leg_handle = put(base, leg);
    };
    REQUIRE(host.configure_native(arm_spec("n13-scope-authority")).status
            == NativeSetupStatus::Applied);
    const std::vector<Bar> bars = {
        ohlc(0, 100.0, 100.0, 100.0, 100.0),
        ohlc(1, 100.0, 100.5,  99.5, 100.0),
        ohlc(2, 100.0, 100.5,  99.5, 100.0),
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    const auto armed = events<no::ArmedEvent>(host);
    REQUIRE(armed.size() == 1);
    CHECK(armed[0].definition->handle == leg_handle);
    CHECK(std::holds_alternative<no::Wait>(armed[0].before));
    const auto* book = std::get_if<no::BookClose>(&armed[0].after);
    REQUIRE(book != nullptr);
    CHECK(book->side == no::Side::Short);
    CHECK(book->cycle == host.cycle());
    CHECK(book->binding_event.ordinal == armed[0].ordinal);
    // The installed level is the owner's fill + 3, and the armed child is a
    // working order from its arm on.
    const auto* stop = std::get_if<no::Stop>(&armed[0].definition->request.trigger);
    REQUIRE(stop != nullptr);
    CHECK(stop->price == 103.0);
    const auto working = host.native_working_requests();
    REQUIRE(working.size() == 1);
    CHECK(working[0].definition->handle == leg_handle);
    CHECK(host.physical_position().signed_units == -2.0);
}

// ── Acceptance rules ────────────────────────────────────────────────────
void acceptance_rules() {
    Host host;
    bool probed = false;
    host.calculation = [&](Host& base) {
        if (probed) return;
        probed = true;
        const auto parent = put(base, [] {
            no::Request entry = tx(1.0, "entry");
            entry.trigger = no::Limit{50.0};          // rests for the whole run
            return entry;
        }());
        const auto status = [&](const no::Request& request) { return base.submit(request); };
        no::Request host_close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "c", ""};
        host_close.trigger = no::Stop{90.0};

        // The established refusal stands: an owner-lot child has no
        // host-sized quantity.
        host_close.owner = no::WaitForApplied{parent};
        auto out = status(host_close);
        CHECK(out.status == no::SubmitStatus::Rejected);
        CHECK(out.reason && *out.reason == no::RequestRejectReason::InvalidOwner);

        // Under Book it is accepted.
        host_close.owner = no::WaitForApplied{parent, no::NativeArmVisibility::Working,
                                              no::NativeArmFirstMatch::AtArmPrint,
                                              no::NativeArmScope::Book};
        out = status(host_close);
        CHECK(out.status == no::SubmitStatus::Accepted);

        // A waiting transaction closes nothing and may not claim a book.
        no::Request add = tx(1.0, "add");
        add.owner = no::WaitForApplied{parent, no::NativeArmVisibility::Working,
                                       no::NativeArmFirstMatch::AtArmPrint,
                                       no::NativeArmScope::Book};
        out = status(add);
        CHECK(out.status == no::SubmitStatus::Rejected);
        CHECK(out.reason && *out.reason == no::RequestRejectReason::InvalidOwner);
        // ...but it may name the first-match rule.
        add.owner = no::WaitForApplied{parent, no::NativeArmVisibility::Working,
                                       no::NativeArmFirstMatch::AfterArmPrint};
        CHECK(status(add).status == no::SubmitStatus::Accepted);

        // An unknown value is refused, never read as the default.
        no::Request reduce_leg = reduce(1.0, "r");
        reduce_leg.trigger = no::Stop{90.0};
        reduce_leg.owner = no::WaitForApplied{parent, no::NativeArmVisibility::Working,
                                              static_cast<no::NativeArmFirstMatch>(7)};
        out = status(reduce_leg);
        CHECK(out.status == no::SubmitStatus::Rejected);
        CHECK(out.reason && *out.reason == no::RequestRejectReason::InvalidOwner);
        reduce_leg.owner = no::WaitForApplied{parent, no::NativeArmVisibility::Working,
                                              no::NativeArmFirstMatch::AtArmPrint,
                                              static_cast<no::NativeArmScope>(7)};
        out = status(reduce_leg);
        CHECK(out.status == no::SubmitStatus::Rejected);
        CHECK(out.reason && *out.reason == no::RequestRejectReason::InvalidOwner);
    };
    run(host, arm_spec("n13-acceptance"), {100.0, 100.0, 100.0});
    CHECK(host.last_error().empty());
    CHECK(probed);
}

// ── Continuation identity ───────────────────────────────────────────────
// Both options fold only when set: a default-spelled child keeps the identity
// it had before the options existed, and each non-default value moves it.
void options_fold_only_when_set() {
    const auto identity = [&](std::optional<no::WaitForApplied> spelled, const char* key) {
        Host host;
        bool placed = false;
        host.calculation = [&](Host& base) {
            if (placed) return;
            placed = true;
            no::Request entry = tx(1.0, "entry");
            entry.trigger = no::Limit{50.0};          // never fills: nothing arms
            const auto parent = put(base, entry);
            no::Request leg = reduce(1.0, "leg");
            leg.trigger = no::Stop{40.0};
            no::WaitForApplied owner = spelled ? *spelled : no::WaitForApplied{};
            owner.parent = parent;
            leg.owner = owner;
            put(base, leg);
        };
        run(host, arm_spec(key), {100.0, 100.0, 100.0});
        CHECK(host.last_error().empty());
        return host.native_continuation_hash();
    };
    const auto plain = identity(std::nullopt, "n13-fold");
    no::WaitForApplied explicit_defaults;
    explicit_defaults.first_match = no::NativeArmFirstMatch::AtArmPrint;
    explicit_defaults.scope = no::NativeArmScope::OwnerLot;
    CHECK(identity(explicit_defaults, "n13-fold") == plain);
    no::WaitForApplied after;
    after.first_match = no::NativeArmFirstMatch::AfterArmPrint;
    no::WaitForApplied book;
    book.scope = no::NativeArmScope::Book;
    no::WaitForApplied hidden;
    hidden.visibility = no::NativeArmVisibility::PendingUntilArmed;
    const auto after_id = identity(after, "n13-fold");
    const auto book_id = identity(book, "n13-fold");
    const auto hidden_id = identity(hidden, "n13-fold");
    CHECK(after_id != plain);
    CHECK(book_id != plain);
    // Tagged folds: no option can read as another.
    CHECK(after_id != book_id);
    CHECK(after_id != hidden_id);
    CHECK(book_id != hidden_id);
}

}  // namespace

int main() {
    test("first match after the arm print", first_match_after_the_arm_print);
    test("first match at a discrete print", first_match_at_a_discrete_print);
    test("book scope covers a later add", book_scope_covers_a_later_add);
    test("book scope arms into the book authority", book_scope_arms_into_the_book_authority);
    test("acceptance rules", acceptance_rules);
    test("options fold only when set", options_fold_only_when_set);
    std::printf("%s native arm options: %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
