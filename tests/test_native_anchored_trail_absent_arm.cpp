// R5 follow-up lane E1: an anchored trail's arm threshold is supplied by the
// owner's fill, so an ABSENT arm_price is its natural spelling.
//
// Before this lane a leg whose trigger was `Trail{offset, std::nullopt}` under
// a FromOwnerFill anchor was refused with RequestRejectReason::InvalidTrigger
// (src/native_order.cpp, validate_levels): the anchored trail was the one
// trigger kind whose placeholder had to be typed out, because Limit::price
// and Stop::price default to the 0.0 the anchored form wants while
// std::optional<double>'s own default is nullopt. Lane L14-B hit exactly that
// building the six-feature host from the page alone.
//
// Two facts in the tree say the placeholder is write-only ceremony:
//
//   1. install_anchored_level() assigns `trail->arm_price = level`
//      UNCONDITIONALLY at the arm. It never reads the prior value, so a
//      placeholder and an absent arm materialize the same definition.
//   2. An anchored request must be owned by WaitForApplied (InvalidOwner
//      otherwise), so its authority is Wait until the arm;
//      eligibility_facts() sets facts.waiting from that and the consumer's
//      candidate scan skips a waiting request outright. The pre-arm arm_price
//      is therefore unreachable by the matcher under EITHER visibility.
//
// This unit proves both ends of that: the executed outcome of the two
// spellings is identical, in batch AND on a stream. One thing separates them
// while the leg waits -- the continuation hash folds arm_price.has_value() of
// every live request by design. Until the v19 value epoch it separated them
// for good, because continuation_hash() folded the command HISTORY, whose
// acceptance row keeps the definition as submitted; since v19
// (native-consumer/v9) it folds live state only, so the two spellings share
// one continuation once the arm has installed the same definition. The Pine
// adapter's placeholder (src/source/pine_adapter.cpp relative_leg_shapes)
// now moves only a digest read while a leg waits.
//
// The rule the lane states on the page is pinned here too: an absent arm is
// accepted, a WRITTEN non-placeholder arm is still refused, exactly as a
// written Limit/Stop price under an anchor is -- that value is a
// contradiction, not an omission, because the arm would silently overwrite it.
#include <pineforge/native_host.hpp>

#include "native_current_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace r4_test;

namespace {

constexpr double kTick = 0.01;
// +300 ticks from the parent's fill: the trail arms at 103.00.
constexpr double kArmTicks = 300.0;
constexpr double kTrailOffset = 0.50;

NativeRunSpec e1_spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "TEST:E1";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000;
    s.point_value = 1;
    s.account_fx = 1;
    s.price_tick = kTick;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0;
    return s;
}

// A rise through the 103.00 arm, an extreme at 104.00, then a retrace through
// the 103.50 trailing stop. Every price is a binary-exact multiple of 0.01.
std::vector<Bar> tape() {
    std::vector<Bar> bars;
    auto push = [&](double o, double h, double l, double c) {
        bars.push_back({o, h, l, c, 1.0, T + static_cast<std::int64_t>(bars.size()) * 60000});
    };
    push(100.0, 100.0, 100.0, 100.0);        // 0: entry + leg submitted
    push(100.0, 100.5, 99.5, 100.25);        // 1: the market entry fills @100.00
    push(100.25, 101.0, 100.0, 100.75);      // 2
    push(100.75, 102.0, 100.5, 101.75);      // 3
    push(101.75, 103.5, 101.5, 103.25);      // 4: crosses 103.00 -> the trail arms
    push(103.25, 104.0, 103.0, 103.5);       // 5: the extreme, best = 104.00
    push(103.5, 103.6, 103.0, 103.2);        // 6: through 104.00 - 0.50 -> the exit
    push(103.2, 103.4, 103.0, 103.2);        // 7
    return bars;
}

struct Observed {
    bool accepted = false;
    std::optional<no::RequestRejectReason> reason;
    std::size_t armed = 0;
    std::optional<double> installed;   // the arm_price the ArmedEvent carries
    std::uint64_t waiting_hash = 0;    // after acceptance, before the arm
    std::uint64_t final_hash = 0;
    std::vector<std::string> applied;
    std::vector<std::string> trades;
    std::string error;
};

// One run of the shape. `placeholder` picks the spelling under test; `stream`
// drives the same tape through stream_begin/stream_push_bar instead of run().
Observed observe(bool placeholder, bool stream, const char* key) {
    Observed out;
    Host host;
    no::RequestHandle parent{};
    int bar = -1;
    host.calculation = [&](Host& base) {
        ++bar;
        if (bar != 0) return;
        const auto entry = base.submit(tx(2.0, "L"));
        if (entry.status != no::SubmitStatus::Accepted || !entry.handle) {
            out.error = "entry rejected";
            return;
        }
        parent = *entry.handle;
        // The bracket child Pine's relative exits lower to: hidden until the
        // fill, starting after the arm print, closing the book.
        no::Request leg{no::Reduce{no::OwnerOpenedUnits{}}, "T", "bracket"};
        leg.owner = no::WaitForApplied{parent, no::NativeArmVisibility::PendingUntilArmed,
                                       no::NativeArmFirstMatch::AfterArmPrint,
                                       no::NativeArmScope::Book};
        leg.anchor = no::FromOwnerFill{kArmTicks, /*ticks=*/true};
        no::Trail trail;
        trail.offset = kTrailOffset;
        if (placeholder) trail.arm_price = 0.0;
        leg.trigger = trail;
        const auto placed = base.submit(leg);
        out.accepted = placed.status == no::SubmitStatus::Accepted;
        out.reason = placed.reason;
        // Still waiting: the entry is a market order that fills on the next
        // bar's open, so nothing has armed yet.
        out.waiting_hash = base.native_continuation_hash();
    };

    const auto bars = tape();
    if (host.configure_native(e1_spec(key)).status != NativeSetupStatus::Applied) {
        out.error = "configure: " + host.last_error();
        return out;
    }
    if (stream) {
        if (!host.stream_begin(bars.data(), 1, "1", "1")) {
            out.error = "stream_begin: " + host.last_error();
            return out;
        }
        for (std::size_t i = 1; i < bars.size(); ++i) {
            if (!host.stream_push_bar(bars[i])) {
                out.error = "stream_push_bar " + std::to_string(i) + ": " + host.last_error();
                return out;
            }
        }
        if (!host.stream_end(false)) {
            out.error = "stream_end: " + host.last_error();
            return out;
        }
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    if (out.error.empty()) out.error = host.last_error();

    for (const auto& row : events<no::ArmedEvent>(host)) {
        if (!row.definition) continue;
        ++out.armed;
        if (const auto* trail = std::get_if<no::Trail>(&row.definition->request.trigger)) {
            out.installed = trail->arm_price;
        }
    }
    for (const auto& event : events<no::ExecutionAppliedEvent>(host)) {
        char line[192];
        std::snprintf(line, sizeof line, "%s @%.17g raw=%.17g closed=%.17g opened=%.17g",
                      event.request().label.c_str(), event.resolved_price, event.raw_price,
                      event.closed_units, event.opened_units);
        out.applied.emplace_back(line);
    }
    for (const auto& trade : host.rows()) {
        char line[192];
        std::snprintf(line, sizeof line, "%s->%s %.17g@%.17g -> %.17g pnl=%.17g",
                      trade.entry_id.c_str(), trade.exit_id.c_str(), trade.qty,
                      trade.entry_price, trade.exit_price, trade.pnl);
        out.trades.emplace_back(line);
    }
    out.final_hash = host.native_continuation_hash();
    return out;
}

void report(const char* what, const Observed& run) {
    std::printf("  %s: accepted=%d armed=%zu installed=%s applied=%zu trades=%zu err='%s'\n", what,
                run.accepted ? 1 : 0, run.armed,
                run.installed ? std::to_string(*run.installed).c_str() : "(none)",
                run.applied.size(), run.trades.size(), run.error.c_str());
    for (const auto& row : run.applied) std::printf("    applied %s\n", row.c_str());
    for (const auto& row : run.trades) std::printf("    trade   %s\n", row.c_str());
}

// ── 1. An absent arm is accepted and arms at the owner's fill ───────────
void absent_arm_is_accepted_and_arms() {
    const Observed absent = observe(/*placeholder=*/false, /*stream=*/false, "e1-absent");
    report("absent", absent);
    CHECK(absent.error.empty());
    CHECK(absent.accepted);
    CHECK(!absent.reason.has_value());
    // The arm installed the level the anchor resolves to: fill 100.00 plus
    // 300 ticks of 0.01.
    REQUIRE(absent.armed == 1);
    REQUIRE(absent.installed.has_value());
    near(*absent.installed, 103.0);
    // And the leg then traded: the trail rode to the 104.00 extreme and
    // exited 0.50 under it.
    REQUIRE(absent.trades.size() == 1);
    CHECK(absent.applied.size() == 2);
}

// ── 2. The placeholder and the absent arm execute identically ──────────
void the_placeholder_is_ceremony() {
    const Observed placeholder = observe(true, false, "e1-batch");
    const Observed absent = observe(false, false, "e1-batch");
    report("placeholder", placeholder);
    CHECK(placeholder.error.empty() && absent.error.empty());
    REQUIRE(placeholder.accepted && absent.accepted);
    REQUIRE(placeholder.installed.has_value() && absent.installed.has_value());
    // install_anchored_level() overwrites the field, so the two spellings
    // materialize the SAME level -- bitwise, not merely close.
    CHECK(*placeholder.installed == *absent.installed);
    CHECK(placeholder.armed == absent.armed);
    REQUIRE(placeholder.applied.size() == absent.applied.size());
    for (std::size_t i = 0; i < placeholder.applied.size(); ++i) {
        CHECK(placeholder.applied[i] == absent.applied[i]);
        if (placeholder.applied[i] != absent.applied[i]) {
            std::printf("    placeholder %s\n    absent      %s\n",
                        placeholder.applied[i].c_str(), absent.applied[i].c_str());
        }
    }
    REQUIRE(placeholder.trades.size() == absent.trades.size());
    for (std::size_t i = 0; i < placeholder.trades.size(); ++i) {
        CHECK(placeholder.trades[i] == absent.trades[i]);
    }
}

// ── 3. The same identity on a stream ────────────────────────────────────
void the_placeholder_is_ceremony_on_a_stream() {
    const Observed placeholder = observe(true, /*stream=*/true, "e1-stream");
    const Observed absent = observe(false, /*stream=*/true, "e1-stream");
    report("stream/absent", absent);
    CHECK(placeholder.error.empty() && absent.error.empty());
    REQUIRE(placeholder.accepted && absent.accepted);
    REQUIRE(placeholder.installed.has_value() && absent.installed.has_value());
    CHECK(*placeholder.installed == *absent.installed);
    near(*absent.installed, 103.0);
    REQUIRE(placeholder.applied.size() == absent.applied.size());
    for (std::size_t i = 0; i < placeholder.applied.size(); ++i) {
        CHECK(placeholder.applied[i] == absent.applied[i]);
    }
    REQUIRE(placeholder.trades.size() == absent.trades.size());
    for (std::size_t i = 0; i < placeholder.trades.size(); ++i) {
        CHECK(placeholder.trades[i] == absent.trades[i]);
    }
    // The stream is the batch: the driving mode does not change the arm.
    const Observed batch = observe(false, false, "e1-stream");
    REQUIRE(batch.trades.size() == absent.trades.size());
    for (std::size_t i = 0; i < batch.trades.size(); ++i) {
        CHECK(batch.trades[i] == absent.trades[i]);
    }
}

// ── 4. The spelling is a continuation-identity choice while the leg waits ──
void the_spelling_is_durable_identity() {
    const Observed placeholder = observe(true, false, "e1-hash");
    const Observed absent = observe(false, false, "e1-hash");
    // hash_trigger() folds arm_price.has_value(), so a waiting leg's digest
    // tells the two spellings apart.
    CHECK(placeholder.waiting_hash != absent.waiting_hash);
    // expectation corrected: final_hash differing -> final_hash equal,
    // because v19 folds live state only (native-consumer/v9). The arm
    // installs one definition for both spellings (scenario 2 proves the
    // executed outcome is identical), and the command history whose
    // acceptance row kept the definition AS SUBMITTED is no longer an input
    // (lane E1 had corrected it the other way for exactly that history fold).
    CHECK(placeholder.final_hash == absent.final_hash);
    // The difference is the spelling and nothing else: the same spelling run
    // twice agrees on both digests.
    const Observed again = observe(false, false, "e1-hash");
    CHECK(again.waiting_hash == absent.waiting_hash);
    CHECK(again.final_hash == absent.final_hash);
    std::printf("  waiting: placeholder=%llu absent=%llu   final: %llu / %llu\n",
                static_cast<unsigned long long>(placeholder.waiting_hash),
                static_cast<unsigned long long>(absent.waiting_hash),
                static_cast<unsigned long long>(placeholder.final_hash),
                static_cast<unsigned long long>(absent.final_hash));
}

// ── 5. A WRITTEN arm under an anchor is still refused ──────────────────
void a_written_arm_is_still_a_contradiction() {
    // The rule the page states: the anchor supplies the arm, so omitting it
    // is the spelling and writing a level is the error. A written level would
    // be silently overwritten at the arm, which is exactly why Limit::price
    // and Stop::price are refused when non-zero under an anchor.
    Host host;
    no::SubmitResult written, absent, placeholder;
    host.beginning = [&](Host& base) {
        const auto parent = put(base, tx(1.0, "L"));
        auto leg = [&](std::optional<double> arm) {
            no::Request request{no::Reduce{no::OwnerOpenedUnits{}}, "T", "bracket"};
            request.owner = no::WaitForApplied{parent,
                                               no::NativeArmVisibility::PendingUntilArmed};
            request.anchor = no::FromOwnerFill{kArmTicks, /*ticks=*/true};
            no::Trail trail;
            trail.offset = kTrailOffset;
            trail.arm_price = arm;
            request.trigger = trail;
            return request;
        };
        written = base.submit(leg(103.0));
        placeholder = base.submit(leg(0.0));
        absent = base.submit(leg(std::nullopt));
    };
    run(host, e1_spec("e1-written"), {100.0});
    completed(host);
    CHECK(written.status == no::SubmitStatus::Rejected);
    REQUIRE(written.reason.has_value());
    CHECK(*written.reason == no::RequestRejectReason::InvalidTrigger);
    // Both omissions of a level are accepted; the placeholder stays legal so
    // every host that already spells it keeps working.
    CHECK(placeholder.status == no::SubmitStatus::Accepted);
    CHECK(absent.status == no::SubmitStatus::Accepted);
    if (absent.status != no::SubmitStatus::Accepted && absent.reason) {
        std::printf("  absent rejected with reason %d\n", static_cast<int>(*absent.reason));
    }
}

}  // namespace

int main() {
    test("absent-arm-accepted", absent_arm_is_accepted_and_arms);
    test("placeholder-is-ceremony", the_placeholder_is_ceremony);
    test("placeholder-is-ceremony-stream", the_placeholder_is_ceremony_on_a_stream);
    test("spelling-is-durable-identity", the_spelling_is_durable_identity);
    test("written-arm-refused", a_written_arm_is_still_a_contradiction);
    std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
