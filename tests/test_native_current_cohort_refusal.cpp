// R5 lane PAR-ORDERS-2: a synchronous current execution refuses a request
// bound to a host roster (BindCohort) instead of failing the run.
//
// validate_current_execution admitted a BindCohort owner (R4 slice C) while
// the evaluation's current shape (native_order.cpp current_shape) admits only
// Independent, BindOpening and BindOpenings owners. A cohort close whose
// roster held a live lot therefore passed validation, took its point, came
// back from the evaluation unevaluated (NotEligible) and failed the run with
// "native current evaluated allowance mismatch". The documented contract
// (native-engine.md "Selected exposure and current execution") never listed
// BindCohort; the validation now refuses it as UnsupportedRequest, before a
// point is taken, whatever its roster holds.
//
// Witnessed here on the kernel alone:
//   1. a host-sized cohort close whose roster holds a live lot: inspect and
//      execute both answer UnsupportedRequest, nothing is taken or recorded
//      (continuation hash unchanged), the run stays Running, and the request
//      stays live and fills at the next open like any queued market close;
//   2. a cohort close whose roster holds no live lot: the same refusal (it was
//      UnreadyOwner), and it also waits and fills once the roster's entry has.
// A fixed roster (BindOpenings) of the same lot still executes at once.
//
// Fail-before, this TU against the lane's base library (6945fc19): case 1's
// execute_current throws std::logic_error("native current evaluated allowance
// mismatch") and the run ends Failed -- the first failure is
//   FAIL test_native_current_cohort_refusal.cpp:<line>: !host.threw
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                         \
    ++checks;                                                                    \
    if (!(expr)) {                                                               \
        ++failures;                                                              \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);            \
    }                                                                            \
} while (0)

constexpr std::int64_t kT = 1736121600000LL;

Bar bar(std::int64_t timestamp, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, timestamp};
}

NativeRunSpec spec_for(const char* key) {
    NativeRunSpec spec;
    spec.event_retention = NativeEventRetention::Full;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "PA2:COHORT";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    return spec;
}

no::Request market(double units, const char* label) {
    no::Request request;
    request.intent = no::Transact{units};
    request.label = label;
    return request;
}

no::Request cohort_close(no::CohortHandle cohort, const char* label) {
    no::Request request;
    request.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt};
    request.owner = no::BindCohort{cohort};
    request.label = label;
    return request;
}

std::vector<no::ExecutionAppliedEvent> applied(const NativeStrategyHost& host, const char* label) {
    std::vector<no::ExecutionAppliedEvent> out;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* event = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            if (event->request().label == label) out.push_back(*event);
        }
    }
    return out;
}

class CohortHost final : public NativeStrategyHost {
public:
    int bars = 0;
    bool threw = false;
    std::string thrown;
    std::optional<NativeCurrentRefusal> live_preview;
    std::optional<NativeCurrentRefusal> live_result;
    std::optional<NativeCurrentRefusal> empty_result;
    bool hash_unchanged = false;
    bool fixed_roster_applied = false;
    no::CohortHandle live_cohort{};
    no::CohortHandle empty_cohort{};
    no::RequestHandle held{};

    // A host-sized close answers the whole scope it is bound to.
    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return {facts.default_resolved_price, facts.scope_exposure_units,
                    no::OpeningShape::Transact};
        }
        return {facts.default_resolved_price, std::nullopt, no::OpeningShape::Transact};
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars;
        if (bars == 1) {
            // Two lots on the live roster, filled at bar 2's open.
            live_cohort = cohort_open();
            const auto first = submit(market(1.0, "live-first"));
            const auto second = submit(market(1.0, "live-second"));
            CHECK(first.handle.has_value() && second.handle.has_value());
            if (first.handle) { cohort_add(live_cohort, *first.handle); held = *first.handle; }
            if (second.handle) cohort_add(live_cohort, *second.handle);
            // A roster whose one entry rests below the market until bar 4.
            empty_cohort = cohort_open();
            auto resting = market(1.0, "empty-entry");
            resting.trigger = no::Limit{97.0};
            const auto entry = submit(resting);
            CHECK(entry.handle.has_value());
            if (entry.handle) cohort_add(empty_cohort, *entry.handle);
        } else if (bars == 2) {
            // 1. The live roster: refused, nothing taken.
            const auto accepted = submit(cohort_close(live_cohort, "cohort-close"));
            CHECK(accepted.handle.has_value());
            if (!accepted.handle) return;
            const NativeCurrentExecution command{*accepted.handle, NativeCurrentPriceRule::NearestTick};
            const auto before = native_continuation_hash();
            live_preview = inspect_current_execution(command).refusal;
            try {
                const auto result = execute_current(command);
                if (const auto* refusal = std::get_if<NativeCurrentRefusal>(&result))
                    live_result = *refusal;
            } catch (const std::exception& e) {
                threw = true;
                thrown = e.what();
            }
            hash_unchanged = native_continuation_hash() == before;
            // 2. The roster with no live lot: the same refusal.
            const auto waiting = submit(cohort_close(empty_cohort, "empty-close"));
            CHECK(waiting.handle.has_value());
            if (waiting.handle) {
                try {
                    const auto result = execute_current(
                        {*waiting.handle, NativeCurrentPriceRule::NearestTick});
                    if (const auto* refusal = std::get_if<NativeCurrentRefusal>(&result))
                        empty_result = *refusal;
                } catch (const std::exception& e) {
                    threw = true;
                    thrown = e.what();
                }
            }
        } else if (bars == 3 && held.incarnation != 0) {
            // A fixed roster executes at once: nothing live remains on it
            // once the cohort close filled at this bar's open, so reopen one.
            const auto lot = submit(market(1.0, "fixed-entry"));
            CHECK(lot.handle.has_value());
        } else if (bars == 4) {
            no::Request close;
            close.intent = no::Flatten{};
            close.label = "fixed-close";
            const auto opening = applied(*this, "fixed-entry");
            if (!opening.empty()) {
                close.owner = no::BindOpenings{{opening.front().handle()},
                                               opening.front().cycle_after};
                const auto accepted = submit(close);
                if (accepted.handle) {
                    const auto result = execute_current(
                        {*accepted.handle, NativeCurrentPriceRule::NearestTick});
                    fixed_roster_applied = std::holds_alternative<no::ExecutionAppliedEvent>(result);
                }
            }
        }
    }
};

}  // namespace

int main() {
    CohortHost host;
    CHECK(host.configure_native(spec_for("pa2-cohort-refusal")).status == NativeSetupStatus::Applied);
    const Bar bars[] = {
        bar(kT, 100, 100, 100, 100),
        bar(kT + 60000, 101, 101, 101, 101),
        bar(kT + 120000, 102, 102, 102, 102),
        bar(kT + 180000, 99, 99, 96, 98),
        bar(kT + 240000, 98, 98, 98, 98),
        bar(kT + 300000, 98, 98, 98, 98),
    };
    host.run(bars, 6);

    // 1. Refused as UnsupportedRequest, before any point was taken.
    CHECK(!host.threw);
    if (host.threw) std::printf("  execute_current threw: %s\n", host.thrown.c_str());
    CHECK(host.live_preview == NativeCurrentRefusal::UnsupportedRequest);
    CHECK(host.live_result == NativeCurrentRefusal::UnsupportedRequest);
    CHECK(host.hash_unchanged);
    CHECK(host.native_state().kind != NativeLifecycleKind::Failed);
    // ... and still live: it fills at bar 3's open like a queued market close,
    // closing both lots of its roster.
    const auto cohort_fills = applied(host, "cohort-close");
    CHECK(cohort_fills.size() == 1);
    if (!cohort_fills.empty()) {
        CHECK(std::fabs(cohort_fills.front().closed_units - 2.0) < 1e-12);
        CHECK(std::fabs(cohort_fills.front().resolved_price - 102.0) < 1e-12);
        CHECK(cohort_fills.front().cursor.point.provenance != NativePriceProvenance::CurrentExecution);
    }
    // 2. A roster with no live lot: the same refusal (UnreadyOwner before),
    // and the close fills once its entry has (the limit at 97 on bar 4).
    CHECK(host.empty_result == NativeCurrentRefusal::UnsupportedRequest);
    CHECK(applied(host, "empty-entry").size() == 1);
    CHECK(applied(host, "empty-close").size() == 1);
    // A fixed roster of a live lot still executes at once.
    CHECK(host.fixed_roster_applied);
    CHECK(host.native_state().kind != NativeLifecycleKind::Failed);
    std::printf("%s native current cohort refusal: %d checks, %d failures\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
