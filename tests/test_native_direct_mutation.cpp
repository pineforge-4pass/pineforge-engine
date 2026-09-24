// R5 lane L3: the order core's direct forms against the prepare/install pairs
// they stand for, bit for bit, after every command.
//
// WorkingRequestCore::apply_* make the checks of the prepare_* they are named
// after, in the same order, and then write in place: the live row is updated
// field by field where it stands, each event is appended to the journal once,
// and no token, plan or row copy stands between the checks and the write. The
// consumer uses them wherever it installed what it prepared straight away;
// NativeExecutionConsumer::set_direct_mutation(false) restores the prepare/
// install pairs everywhere, and that switch is what this row turns.
//
// Part A drives two bare cores -- one through the pairs, one through the
// direct forms -- with the same randomized command stream: every command and
// mutation the core serves (submit, replace, cancel, evaluation, the five
// trigger transitions, terms, no-effect, match refusal, group effect, owner
// arm with and without an anchored level and its restatement, bound expiry,
// parent terminal, margin and risk receipts), executions through the one token
// both paths share, allowance refreshes, cohort commands and journal
// retirement; observations and proposals that are consistent and that are
// not; counters that regress, collide and skip. After every step both answer
// the same (the installed range or the command result, the NoChange reason,
// the PreparationError, the exception's type and text) and both cores hold the
// same state: every retained event field by field, every live row, the ordinal
// index, the receipts, the chain index, the rosters and the counters
// (native_order_full_fold.hpp). A token prepared before a step and installed
// after it answers the same on both cores, so the direct forms move the
// staleness epoch exactly as a prepare and its install do.
//
// Part B runs whole books on the consumer twice, the direct forms on and off,
// and compares after every command the host issues and at every bar and fill:
// the request core (the same full fold), the continuation hash and the broker
// hash; at the end every event of the run field by field, every trade and the
// fixture's own outcome. The books are PERF-K3's randomized working book
// (native_match_book_fixture.hpp), PERF-L2's fused-settlement host
// (native_fused_settlement_fixture.hpp: fee forms, FX, the margin model,
// streams, excursion owners, reversals, selections) and this row's own
// scripted host, which adds what neither of them issues: anchored legs with a
// restating host, deferred group reductions (chains the journal window must
// keep), retaining replaces, legs pending until armed, kernel-sized openings
// frozen at acceptance, bound openings, the risk block's flatten, and
// executions against the current price -- each under the Window retention a
// run keeps by default and under Full.
//
// Fail-before: this TU does not compile against 10f20197 (no apply_* and no
// set_direct_mutation); the first diagnostic is recorded in the lane report.
//
// Source-free: it also runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"
#include "native_fused_settlement_fixture.hpp"
#include "native_match_book_fixture.hpp"
#include "native_order_full_fold.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

// ===========================================================================
// Part A: two bare cores, staged against direct.
// ===========================================================================
namespace core_twins {

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed)
        : state(seed * 0x9E3779B97F4A7C15ull ^ 0x5DEECE66Dull) {
        if (state == 0) state = 1;
    }
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    int between(int lo, int hi) { return lo + below(hi - lo + 1); }
    bool percent(int p) { return below(100) < p; }
    double quarter(int lo, int hi) { return static_cast<double>(between(lo, hi)) * 0.25; }
};

// What one step answered, as a comparable value.
struct Answer {
    int kind = -1;  // 0 installed, 1 no change, 2 preparation error, 3 install error, 4 thrown
    std::uint64_t value = 0;
    std::string text;
};
bool same(const Answer& a, const Answer& b) {
    return a.kind == b.kind && a.value == b.value && a.text == b.text;
}

Answer installed(const no::EventRange& range, std::uint64_t result = 0) {
    full_fold::Fold f;
    f.u(range.first_index);
    f.u(range.count);
    f.u(result);
    return {0, f.h, {}};
}
Answer answer(const no::NoChange& none) {
    return {1, static_cast<std::uint64_t>(none.reason), {}};
}
Answer answer(const no::PreparationError& error) {
    full_fold::Fold f;
    f.e(error.code);
    full_fold::fold(f, error.cause);
    full_fold::fold(f, error.target);
    return {2, f.h, {}};
}
Answer answer(no::InstallError error) { return {3, static_cast<std::uint64_t>(error), {}}; }
Answer thrown(const std::exception& e) {
    return {4, 0, std::string(typeid(e).name()) + ": " + e.what()};
}

std::uint64_t result_value(const no::SubmitResult& r) {
    full_fold::Fold f;
    f.e(r.status); f.u(r.event_ordinal);
    full_fold::fold_optional(f, r.handle);
    f.b(r.reason.has_value()); if (r.reason) f.e(*r.reason);
    return f.h;
}
std::uint64_t result_value(const no::ReplaceResult& r) {
    full_fold::Fold f;
    f.e(r.status); f.u(r.event_ordinal);
    full_fold::fold_optional(f, r.successor);
    f.b(r.reason.has_value()); if (r.reason) f.e(*r.reason);
    return f.h;
}
std::uint64_t result_value(const no::CancelResult& r) {
    full_fold::Fold f;
    f.e(r.status); f.u(r.event_ordinal);
    return f.h;
}

template <class R>
Answer staged_command(const no::InstalledCommand<R>& installed_command) {
    if (const auto* error = std::get_if<no::InstallError>(&installed_command)) return answer(*error);
    const auto& ok = std::get<no::CommandInstalled<R>>(installed_command);
    return installed(ok.events, result_value(ok.result));
}
template <class R>
Answer direct_command(const no::CommandInstalled<R>& ok) {
    return installed(ok.events, result_value(ok.result));
}

Answer staged_mutation(no::WorkingRequestCore& core,
                       no::Preparation<no::PreparedMutation>&& prepared) {
    if (auto* token = std::get_if<no::PreparedMutation>(&prepared)) {
        const auto result = core.install_mutation(std::move(*token));
        if (const auto* error = std::get_if<no::InstallError>(&result)) return answer(*error);
        return installed(std::get<no::Installed>(result).events);
    }
    if (const auto* none = std::get_if<no::NoChange>(&prepared)) return answer(*none);
    return answer(std::get<no::PreparationError>(prepared));
}
Answer direct_mutation(const no::Preparation<no::Installed>& applied) {
    if (const auto* done = std::get_if<no::Installed>(&applied)) return installed(done->events);
    if (const auto* none = std::get_if<no::NoChange>(&applied)) return answer(*none);
    return answer(std::get<no::PreparationError>(applied));
}

const no::RunIdentity kRun{"l3-direct", 1};

struct Totals {
    long steps = 0;
    long installed = 0;
    long no_change = 0;
    long refused = 0;   // preparation errors
    long thrown = 0;
    long stale_probes = 0;
    long executions = 0;
    long executions_installed = 0;
    long events = 0;
    long retired = 0;
};

class Script {
public:
    Script(std::uint64_t seed, Totals& totals)
        : rng_(seed), seed_(seed), staged_(kRun), direct_(kRun), totals_(totals) {}

    void run(int steps) {
        for (int index = 0; index < steps && failures == 0; ++index) {
            step_index_ = index;
            one_step();
        }
        CHECK(full_fold::core_value(staged_) == full_fold::core_value(direct_));
        totals_.events += static_cast<long>(staged_.history_end());
    }

private:
    // ---- the two cores, one step at a time --------------------------------
    template <class Staged, class Direct>
    void step(const char* name, Staged&& staged_op, Direct&& direct_op) {
        // A token prepared before the step on both cores, installed after it:
        // the two answers must agree (a step that changed the core leaves it
        // stale on both, a step that did not leaves it installable on both).
        std::optional<no::PreparedCancel> staged_probe;
        std::optional<no::PreparedCancel> direct_probe;
        const bool probe = rng_.percent(12) && !staged_.live().empty();
        no::RequestHandle probe_target;
        if (probe) {
            probe_target = staged_.live()[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(staged_.live().size())))].handle();
            std::uint64_t so = next_ordinal(), doo = so;
            try {
                staged_probe.emplace(staged_.prepare_cancel(probe_target, so));
                direct_probe.emplace(direct_.prepare_cancel(probe_target, doo));
            } catch (const std::exception&) {
                staged_probe.reset();
                direct_probe.reset();
            }
        }
        Answer staged_answer;
        Answer direct_answer;
        try {
            staged_answer = staged_op(staged_);
        } catch (const std::exception& e) {
            staged_answer = thrown(e);
        }
        try {
            direct_answer = direct_op(direct_);
        } catch (const std::exception& e) {
            direct_answer = thrown(e);
        }
        ++totals_.steps;
        switch (staged_answer.kind) {
        case 0: ++totals_.installed; break;
        case 1: ++totals_.no_change; break;
        case 2: ++totals_.refused; break;
        case 4: ++totals_.thrown; break;
        default: break;
        }
        const bool answers = same(staged_answer, direct_answer);
        CHECK(answers);
        std::uint64_t staged_value = staged_tracker_.step(staged_);
        std::uint64_t direct_value = direct_tracker_.step(direct_);
        CHECK(staged_value == direct_value);
        if (step_index_ % 50 == 49) {
            staged_value = full_fold::core_value(staged_);
            direct_value = full_fold::core_value(direct_);
            CHECK(staged_value == direct_value);
        }
        if (!answers || staged_value != direct_value) {
            std::fprintf(stderr, "  core twins seed=%llu step=%d op=%s staged=(%d,%llx,%s) direct=(%d,%llx,%s)\n",
                         static_cast<unsigned long long>(seed_), step_index_, name,
                         staged_answer.kind, static_cast<unsigned long long>(staged_answer.value),
                         staged_answer.text.c_str(), direct_answer.kind,
                         static_cast<unsigned long long>(direct_answer.value),
                         direct_answer.text.c_str());
        }
        if (staged_probe && direct_probe) {
            ++totals_.stale_probes;
            const auto staged_late = staged_.install_cancel(std::move(*staged_probe));
            const auto direct_late = direct_.install_cancel(std::move(*direct_probe));
            CHECK(same(staged_command(staged_late), staged_command(direct_late)));
            CHECK(staged_tracker_.step(staged_) == direct_tracker_.step(direct_));
            record_events();
        }
        record_events();
    }

    // Both cores' counters must be distinct variables each, as the consumer's
    // are; the staged core is read for the next free values (the cores agree).
    std::uint64_t next_ordinal() {
        std::uint64_t next = std::max(staged_.last_ordinal(), floor_) + 1;
        if (rng_.percent(3)) next = staged_.last_ordinal();  // regressed or zero
        return next;
    }
    std::uint64_t next_incarnation() {
        std::uint64_t next = staged_.last_incarnation() + 1;
        if (rng_.percent(10)) next += static_cast<std::uint64_t>(rng_.between(1, 3));
        if (rng_.percent(2)) next = staged_.last_incarnation();
        return next;
    }

    // The applied fills, the kernel-origin definitions and the openings the
    // run has produced, read off the staged journal as it grows.
    void record_events() {
        for (std::size_t index = std::max(seen_, staged_.history_base());
             index < staged_.history_end(); ++index) {
            const auto& event = staged_.history_at(index);
            const std::uint64_t ordinal =
                    std::visit([](const auto& payload) { return payload.ordinal; }, event);
            all_events_.push_back(no::EventId{kRun, ordinal});
            if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&event)) {
                applied_.push_back(no::EventId{kRun, ordinal});
                if (applied->opened_units != 0.0) {
                    openings_.push_back(Opening{applied->handle(), applied->cycle_after,
                                                applied->opened_units > 0.0 ? no::Side::Long
                                                                            : no::Side::Short});
                }
            }
            if (const auto* accepted = std::get_if<no::AcceptedEvent>(&event)) {
                if (accepted->definition->origin != no::RequestOrigin::Host) {
                    kernel_definitions_.push_back(accepted->definition);
                }
            }
        }
        seen_ = staged_.history_end();
    }

    // ---- inputs ------------------------------------------------------------
    struct Opening {
        no::RequestHandle handle;
        std::int64_t cycle = 0;
        no::Side side = no::Side::Long;
    };

    no::RequestHandle any_handle() {
        const int pick = rng_.below(100);
        if (pick < 75 && !staged_.live().empty()) {
            return staged_.live()[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(staged_.live().size())))].handle();
        }
        if (pick < 90 && staged_.last_incarnation() > 0) {
            return no::RequestHandle{kRun, static_cast<std::uint64_t>(
                    rng_.between(1, static_cast<int>(staged_.last_incarnation())))};
        }
        if (pick < 95) return no::RequestHandle{no::RunIdentity{"other", 1}, 1};
        return no::RequestHandle{kRun, 0};
    }

    no::Side side() { return rng_.percent(50) ? no::Side::Long : no::Side::Short; }

    no::Trigger trigger(bool anchored) {
        const int pick = rng_.below(100);
        if (anchored) {
            if (pick < 40) return no::Limit{0.0, rng_.percent(10)};
            if (pick < 75) return no::Stop{0.0};
            no::Trail trail;
            trail.offset = rng_.quarter(0, 8);
            if (rng_.percent(50)) trail.arm_price = 0.0;
            return trail;
        }
        if (pick < 20) return no::Market{};
        if (pick < 45) return no::Limit{rng_.quarter(360, 440), rng_.percent(10)};
        if (pick < 65) return no::Stop{rng_.quarter(360, 440)};
        if (pick < 77) {
            const double stop = rng_.quarter(360, 440);
            return no::StopLimit{stop, stop + rng_.quarter(-8, 8)};
        }
        no::Trail trail;
        if (rng_.percent(20)) {
            trail.ticks = no::TrailTicks{static_cast<double>(rng_.between(0, 12))};
        } else {
            trail.offset = rng_.quarter(rng_.percent(3) ? -4 : 0, 12);
        }
        if (rng_.percent(60)) trail.arm_price = rng_.quarter(360, 440);
        if (rng_.percent(15)) trail.best_seed = rng_.quarter(360, 440);
        return trail;
    }

    no::Request request() {
        no::Request r;
        r.label = rng_.percent(50) ? "a" : "a-much-longer-label-than-sso";
        r.comment = rng_.percent(20) ? "c" : "";
        const int owner = rng_.below(100);
        const bool waits = owner < 25 && !staged_.live().empty();
        const int intent = rng_.below(100);
        if (intent < 30) {
            r.intent = no::Transact{(rng_.percent(50) ? 1.0 : -1.0) * rng_.between(1, 3)};
        } else if (intent < 50) {
            const int size = rng_.below(100);
            if (size < 50) r.intent = no::Reduce{no::ExplicitUnits{static_cast<double>(rng_.between(1, 3))}};
            else if (size < 75) r.intent = no::Reduce{no::OwnerOpenedUnits{}};
            else {
                no::ScopeFraction fraction;
                fraction.fraction = rng_.percent(90) ? 0.5 : 1.5;
                fraction.claim = rng_.percent(50) ? no::ScopeClaim::Gross : no::ScopeClaim::NetOfSiblings;
                fraction.basis = rng_.percent(50) ? no::ScopeBasis::AtMatch : no::ScopeBasis::AtAcceptance;
                r.intent = no::Reduce{fraction};
            }
        } else if (intent < 62) {
            r.intent = no::Flatten{};
        } else if (intent < 70) {
            r.intent = no::ReverseTo{(rng_.percent(50) ? 1.0 : -1.0) * rng_.between(1, 3)};
        } else if (intent < 85) {
            no::HostSized sized;
            sized.kind = rng_.percent(50) ? no::HostSizedKind::Open : no::HostSizedKind::Close;
            if (sized.kind == no::HostSizedKind::Open || rng_.percent(5)) sized.side = side();
            r.intent = sized;
        } else {
            no::Sized sized;
            sized.side = side();
            if (rng_.percent(50)) sized.basis = no::CashValue{rng_.percent(95) ? 1000.0 : -1.0};
            else sized.basis = no::EquityFraction{0.1};
            sized.time = rng_.percent(50) ? no::SizeTime::AtMatch : no::SizeTime::AtAcceptance;
            sized.price = static_cast<no::SizePrice>(rng_.below(3));
            sized.grid_policy = rng_.percent(50) ? no::ExecutionGridPolicy::SnapToGrid
                                                 : no::ExecutionGridPolicy::ExplicitUnits;
            r.intent = sized;
        }
        bool anchored = false;
        if (waits) {
            no::WaitForApplied wait;
            wait.parent = staged_.live()[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(staged_.live().size())))].handle();
            wait.visibility = rng_.percent(30) ? no::NativeArmVisibility::PendingUntilArmed
                                               : no::NativeArmVisibility::Working;
            wait.first_match = rng_.percent(30) ? no::NativeArmFirstMatch::AfterArmPrint
                                                : no::NativeArmFirstMatch::AtArmPrint;
            wait.scope = rng_.percent(40) ? no::NativeArmScope::Book : no::NativeArmScope::OwnerLot;
            r.owner = wait;
            anchored = rng_.percent(35);
        } else if (owner < 35 && !openings_.empty()) {
            const auto& opening = openings_[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(openings_.size())))];
            r.owner = no::BindOpening{opening.handle, opening.cycle};
        } else if (owner < 42 && !openings_.empty()) {
            no::BindOpenings bind;
            const auto& first = openings_[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(openings_.size())))];
            bind.cycle = first.cycle;
            for (const auto& opening : openings_) {
                if (opening.cycle == first.cycle && rng_.percent(70)) bind.openings.push_back(opening.handle);
            }
            if (bind.openings.empty()) bind.openings.push_back(first.handle);
            r.owner = bind;
        } else if (owner < 50 && !staged_.cohorts().empty()) {
            r.owner = no::BindCohort{staged_.cohorts()[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(staged_.cohorts().size())))].handle};
        }
        r.trigger = trigger(anchored);
        if (anchored) {
            no::FromOwnerFill from;
            from.offset = rng_.quarter(-12, 12);
            from.ticks = rng_.percent(30);
            from.rounding = static_cast<no::NativeAnchorRounding>(rng_.below(3));
            r.anchor = from;
        }
        if (rng_.percent(12)) r.capacity = no::PointBudget{rng_.percent(90) ? 1.0 : 0.3};
        if (rng_.percent(30)) {
            no::Member member;
            member.group = static_cast<std::uint64_t>(rng_.between(rng_.percent(3) ? 0 : 1, 3));
            member.cohort = rng_.between(0, 1);
            member.effect = rng_.percent(60) ? no::GroupEffect::Cancel : no::GroupEffect::Reduce;
            r.group = member;
        }
        return r;
    }

    no::PositionIdentity position() const {
        if (cycle_ == 0) return no::PositionFlat{};
        return no::PositionNonflat{cycle_, side_};
    }

    no::OpeningObservation observe_opening(const no::RequestHandle& opening, std::int64_t cycle,
                                           no::Side at_side, bool consistent) {
        no::OpeningObservation observation;
        observation.queried_opening = opening;
        observation.queried_cycle = cycle;
        observation.current_position = no::PositionNonflat{cycle, at_side};
        observation.has_live_matching_lot = true;
        if (!consistent) {
            switch (rng_.below(4)) {
            case 0: observation.has_live_matching_lot = false; break;
            case 1: observation.current_position = no::PositionFlat{}; break;
            case 2: observation.queried_cycle = cycle + 1; break;
            default: observation.current_position = no::PositionNonflat{cycle + 1, at_side}; break;
            }
        }
        return observation;
    }

    no::CommandContext command_context(const no::Request& r) {
        no::CommandContext context;
        context.decision_time_ms = time_ms_ - (rng_.percent(20) ? 60000 : 0);
        if (rng_.percent(20)) context.quantity_grid = rng_.percent(95) ? 1.0 : -1.0;
        if (rng_.percent(5)) context.surface = no::CommandSurface::MarketOnly;
        if (rng_.percent(70)) context.price_tick = 0.25;
        if (const auto* bind = std::get_if<no::BindOpening>(&r.owner)) {
            if (rng_.percent(95)) {
                context.opening = observe_opening(bind->opening, bind->cycle,
                                                  opening_side(bind->opening), rng_.percent(85));
            }
        } else if (const auto* binds = std::get_if<no::BindOpenings>(&r.owner)) {
            auto sorted = binds->openings;
            std::sort(sorted.begin(), sorted.end(),
                      [](const no::RequestHandle& a, const no::RequestHandle& b) {
                          return a.incarnation < b.incarnation;
                      });
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
            const bool consistent = rng_.percent(85);
            for (const auto& handle : sorted) {
                context.openings.push_back(observe_opening(
                        handle, binds->cycle, opening_side(sorted.front()), consistent));
            }
        }
        if (const auto* sized = std::get_if<no::Sized>(&r.intent)) {
            if (sized->time == no::SizeTime::AtAcceptance && rng_.percent(90)) {
                context.sizing_units = static_cast<double>(rng_.between(1, 3));
            }
            if (sized->price != no::SizePrice::Resolved && rng_.percent(90)) {
                context.sizing_price = rng_.quarter(380, 420);
            }
            context.sizing_admissible = rng_.percent(92);
        }
        if (const auto* reduce = std::get_if<no::Reduce>(&r.intent)) {
            if (const auto* fraction = std::get_if<no::ScopeFraction>(&reduce->size)) {
                if (fraction->basis == no::ScopeBasis::AtAcceptance) context.sizing_scope = 2.0;
            }
        }
        return context;
    }

    no::Side opening_side(const no::RequestHandle& handle) const {
        for (const auto& opening : openings_) {
            if (opening.handle == handle) return opening.side;
        }
        return no::Side::Long;
    }

    no::MatchCursor cursor() {
        no::MatchCursor c;
        c.point.ordinal = point_ordinal_;
        c.point.interval_index = static_cast<int>(point_ordinal_ % 97);
        c.point.open_ms = time_ms_ - 60000;
        c.point.effective_time_ms = time_ms_;
        c.point.last_traded_close_ms = time_ms_;
        const int provenance = rng_.below(100);
        if (provenance < 45) {
            c.point.provenance = NativePriceProvenance::Confirmed;
            c.point.path_phase = static_cast<NativePathPhase>(rng_.between(1, 4));
        } else if (provenance < 70) {
            c.point.provenance = NativePriceProvenance::ObservedPrint;
        } else if (provenance < 85) {
            c.point.provenance = NativePriceProvenance::ModeledOHLCOpen;
            c.point.path_phase = NativePathPhase::Open;
        } else {
            c.point.provenance = NativePriceProvenance::ModeledOHLCClose;
            c.point.path_phase = NativePathPhase::Close;
        }
        c.t = rng_.percent(50) ? 0.0 : rng_.quarter(0, 4) / 1.0 * 0.25;
        return c;
    }

    no::DriverEligibilityClass driver_for(const no::MatchCursor& c) {
        if (rng_.percent(15)) return static_cast<no::DriverEligibilityClass>(rng_.below(6));
        switch (c.point.provenance) {
        case NativePriceProvenance::ObservedPrint: return no::DriverEligibilityClass::ObservedPrint;
        case NativePriceProvenance::ModeledOHLCOpen: return no::DriverEligibilityClass::ConfirmedOpen;
        case NativePriceProvenance::ModeledOHLCClose:
            return rng_.percent(50) ? no::DriverEligibilityClass::ConfirmedExcursion
                                    : no::DriverEligibilityClass::ConfirmedAfterCalculationClose;
        default:
            return c.point.path_phase == NativePathPhase::Open
                ? no::DriverEligibilityClass::ConfirmedOpen
                : no::DriverEligibilityClass::ConfirmedExcursion;
        }
    }

    no::EvaluationContext evaluation_context(const no::LiveRequest* live) {
        no::EvaluationContext context;
        context.cursor = cursor();
        context.driver_class = driver_for(context.cursor);
        context.existing_matching_bit = rng_.percent(70);
        context.pre_open_birth_eligible = rng_.percent(10);
        if (live && std::holds_alternative<no::CohortClose>(live->authority)) {
            if (rng_.percent(80)) context.cohort_side = side_;
        } else if (rng_.percent(5)) {
            context.cohort_side = side();
        }
        return context;
    }

    // An observation of the book a live request's authority reads, consistent
    // with that authority most of the time.
    no::TargetObservation target_for(const no::LiveRequest* live) {
        no::TargetObservation observation;
        observation.current_position = position();
        const bool consistent = rng_.percent(80);
        if (!live) return observation;
        if (const auto* book = std::get_if<no::BookClose>(&live->authority)) {
            if (consistent) observation.current_position = no::PositionNonflat{book->cycle, book->side};
        } else if (const auto* close = std::get_if<no::OpeningClose>(&live->authority)) {
            observation.opening = observe_opening(close->opening, close->cycle, close->side, consistent);
            observation.current_position = observation.opening->current_position;
        } else if (const auto* closes = std::get_if<no::OpeningsClose>(&live->authority)) {
            for (const auto& handle : closes->openings) {
                auto one = observe_opening(handle, closes->cycle, closes->side, true);
                one.has_live_matching_lot = consistent ? rng_.percent(80) : false;
                observation.openings.push_back(one);
            }
            observation.current_position = no::PositionNonflat{closes->cycle, closes->side};
            if (!consistent && rng_.percent(50) && !observation.openings.empty()) {
                observation.openings.pop_back();
            }
        } else if (std::holds_alternative<no::CohortClose>(live->authority)) {
            for (const auto& opening : openings_) {
                if (opening.cycle != cycle_) continue;
                observation.openings.push_back(observe_opening(opening.handle, cycle_, side_, true));
            }
            if (!consistent) observation.openings.clear();
        } else if (std::holds_alternative<no::UnboundBookClose>(live->authority)) {
            if (!consistent) observation.current_position = no::PositionNonflat{0, side_};
        }
        return observation;
    }

    const no::LiveRequest* find(const no::RequestHandle& handle) const {
        return staged_.find_live(handle);
    }

    // ---- steps ------------------------------------------------------------
    void one_step() {
        const int pick = rng_.below(1000);
        if (pick < 60) return advance_point();
        if (pick < 200) return submit();
        if (pick < 270) return replace();
        if (pick < 310) return cancel();
        if (pick < 450) return evaluation();
        if (pick < 500) return refresh();
        if (pick < 580) return trigger_step();
        if (pick < 600) return terminal_step();
        if (pick < 650) return terms_step();
        if (pick < 760) return execution();
        if (pick < 790) return group_effect();
        if (pick < 850) return owner_applied();
        if (pick < 880) return bound_expiry();
        if (pick < 910) return parent_terminal();
        if (pick < 925) return margin_call();
        if (pick < 935) return risk_event();
        if (pick < 965) return retire();
        return cohort_step();
    }

    void advance_point() {
        floor_ = std::max(staged_.last_ordinal(), floor_) + 1;
        point_ordinal_ = floor_;
        time_ms_ += 60000;
        if (rng_.percent(20)) {
            // The book moves under the requests: a new cycle, a side, or flat.
            const int move = rng_.below(3);
            if (move == 0) cycle_ = 0;
            else if (move == 1) cycle_ = cycle_ + 1;
            else side_ = rng_.percent(50) ? no::Side::Long : no::Side::Short;
        }
    }

    void submit() {
        const no::Request r = request();
        const no::CommandContext context = command_context(r);
        const auto origin = rng_.percent(6) ? no::RequestOrigin::KernelLiquidation
                                            : no::RequestOrigin::Host;
        const std::uint64_t incarnation = next_incarnation();
        const std::uint64_t ordinal = next_ordinal();
        const bool collide = rng_.percent(1);
        step("submit",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t i = incarnation, o = ordinal;
                 auto prepared = core.prepare_submit(r, context, i, collide ? i : o, origin);
                 return staged_command(core.install_submit(std::move(prepared)));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t i = incarnation, o = ordinal;
                 return direct_command(core.apply_submit(r, context, i, collide ? i : o, origin));
             });
    }

    void replace() {
        const no::RequestHandle target = any_handle();
        no::Request r = request();
        const no::CommandContext context = command_context(r);
        no::ReplaceOptions options;
        options.retain_trigger_state = rng_.percent(25);
        if (options.retain_trigger_state) {
            if (const auto* live = find(target)) {
                if (rng_.percent(85)) r.trigger = live->request().trigger;
            }
        }
        const std::uint64_t incarnation = next_incarnation();
        const std::uint64_t ordinal = next_ordinal();
        step("replace",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t i = incarnation, o = ordinal;
                 auto prepared = core.prepare_replace(target, r, context, i, o, options);
                 return staged_command(core.install_replace(std::move(prepared)));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t i = incarnation, o = ordinal;
                 return direct_command(core.apply_replace(target, r, context, i, o, options));
             });
    }

    void cancel() {
        const no::RequestHandle target = any_handle();
        const no::CancelReason reason = rng_.percent(85) ? no::CancelReason::User
                                                         : no::CancelReason::Superseded;
        const std::uint64_t ordinal = next_ordinal();
        step("cancel",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 auto prepared = core.prepare_cancel(target, o, reason);
                 return staged_command(core.install_cancel(std::move(prepared)));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_command(core.apply_cancel(target, o, reason));
             });
    }

    void evaluation() {
        const no::RequestHandle target = any_handle();
        const auto* live = find(target);
        const no::EvaluationContext context = evaluation_context(live);
        const no::TargetObservation observation = target_for(live);
        const std::uint64_t ordinal = next_ordinal();
        step("evaluation",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_evaluation(target, context, observation, o));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_evaluation(target, context, observation, o));
             });
    }

    // Already a direct write in both paths; run on both so allowances exist
    // for executions and so the twins stay one book.
    void refresh() {
        const no::RequestHandle target = any_handle();
        const auto* live = find(target);
        const no::EvaluationContext context = evaluation_context(live);
        const no::TargetObservation observation = target_for(live);
        step("refresh_allowance",
             [&](no::WorkingRequestCore& core) {
                 return Answer{0, core.refresh_allowance(target, context, observation) ? 1u : 0u, {}};
             },
             [&](no::WorkingRequestCore& core) {
                 return Answer{0, core.refresh_allowance(target, context, observation) ? 1u : 0u, {}};
             });
    }

    double level_of(const no::LiveRequest& live) {
        const auto& trigger = live.request().trigger;
        if (const auto* stop = std::get_if<no::Stop>(&trigger)) return stop->price;
        if (const auto* stop_limit = std::get_if<no::StopLimit>(&trigger)) return stop_limit->stop;
        if (const auto* limit = std::get_if<no::Limit>(&trigger)) return limit->price;
        if (const auto* trail = std::get_if<no::Trail>(&trigger)) {
            if (const auto* track = std::get_if<no::TrailTrack>(&live.trigger_state)) return track->best;
            return trail->arm_price ? *trail->arm_price : 400.0;
        }
        return 400.0;
    }

    void trigger_step() {
        const no::RequestHandle target = any_handle();
        const auto* live = find(target);
        const no::MatchCursor at = cursor();
        const double base = live ? level_of(*live) : 400.0;
        const double reached = rng_.percent(3) ? std::nan("") : base + rng_.quarter(-8, 8);
        no::TriggerTransition transition;
        switch (rng_.below(5)) {
        case 0: transition = no::BeginTrailTracking{at, reached}; break;
        case 1: transition = no::ObserveTrailExtremum{at, reached}; break;
        case 2: transition = no::ActivateStop{at, reached}; break;
        case 3: transition = no::ActivateStopLimit{at, reached}; break;
        default: transition = no::ActivateTrail{at, reached}; break;
        }
        const auto driver = driver_for(at);
        std::optional<no::Side> cohort_side;
        if (rng_.percent(20)) cohort_side = side();
        no::ActivationGrid grid;
        if (rng_.percent(30)) {
            grid.price_tick = 0.25;
            grid.half_up = rng_.percent(50);
        }
        if (rng_.percent(40)) grid.ladder_tick = 0.25;
        const std::uint64_t ordinal = next_ordinal();
        step("trigger",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_trigger(target, transition, driver, o,
                                                                   cohort_side, grid));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_trigger(target, transition, driver, o,
                                                           cohort_side, grid));
             });
    }

    void terminal_step() {
        const no::RequestHandle target = any_handle();
        const auto* live = find(target);
        const no::EvaluationContext context = evaluation_context(live);
        const std::uint64_t ordinal = next_ordinal();
        if (rng_.percent(50)) {
            step("no_effect",
                 [&](no::WorkingRequestCore& core) {
                     std::uint64_t o = ordinal;
                     return staged_mutation(core, core.prepare_no_effect(target, context, o));
                 },
                 [&](no::WorkingRequestCore& core) {
                     std::uint64_t o = ordinal;
                     return direct_mutation(core.apply_no_effect(target, context, o));
                 });
            return;
        }
        const auto reason = static_cast<no::MatchRejectReason>(rng_.below(10));
        std::optional<no::ExecutionTerms> attempted;
        if (rng_.percent(50)) {
            attempted = no::ExecutionTerms{rng_.quarter(380, 420), std::nullopt,
                                           no::OpeningShape::Transact};
            if (rng_.percent(50)) attempted->units = 1.0;
        }
        step("match_rejected",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_match_rejected(target, context, reason,
                                                                          attempted, o));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_match_rejected(target, context, reason,
                                                                  attempted, o));
             });
    }

    void terms_step() {
        const no::RequestHandle target = any_handle();
        const auto* live = find(target);
        const no::EvaluationContext context = evaluation_context(live);
        no::TermsResolvedInput input;
        input.price_kind = static_cast<no::NativeCandidatePriceKind>(rng_.below(3));
        input.shared_cursor_collision = rng_.percent(10);
        input.raw_price = rng_.quarter(380, 420);
        input.default_resolved_price = input.raw_price;
        input.terms.resolved_price = input.raw_price;
        const int units = rng_.below(100);
        if (units < 70) input.terms.units = units < 10 ? 0.0 : static_cast<double>(rng_.between(1, 3));
        else if (units < 75) input.terms.units = -1.0;
        const int shape = rng_.below(100);
        input.terms.shape = shape < 75 ? no::OpeningShape::Transact
            : (shape < 88 ? no::OpeningShape::ReverseTo : no::OpeningShape::CloseOpposite);
        const std::uint64_t ordinal = next_ordinal();
        step("terms",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_terms(target, context, input, o));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_terms(target, context, input, o));
             });
    }

    // An execution of a live request whose allowance is this point's: the
    // token pair on the staged core, check_execution and apply_execution on
    // the direct one. Beyond itself, it puts applied fills, openings and
    // closes into the journal for the steps that read them.
    void execution() {
        std::vector<std::size_t> candidates;
        for (std::size_t index = 0; index < staged_.live().size(); ++index) {
            const auto& live = staged_.live()[index];
            const auto* units = std::get_if<no::AllowanceUnits>(&live.allowance);
            const auto* all = std::get_if<no::AllowanceAllScope>(&live.allowance);
            if ((units && units->point_ordinal == point_ordinal_ && units->left > 0.0)
                || (all && all->point_ordinal == point_ordinal_)) {
                candidates.push_back(index);
            }
        }
        // Nothing allowed at this point yet: evaluate something at it first.
        if (candidates.empty()) return rng_.percent(60) ? evaluation() : advance_point();
        const auto& live = staged_.live()[candidates[static_cast<std::size_t>(
                rng_.below(static_cast<int>(candidates.size())))]];
        const no::RequestHandle target = live.handle();
        no::ExecutionProposal proposal;
        proposal.cursor = cursor();
        proposal.cursor.point.ordinal = point_ordinal_;
        proposal.raw_price = rng_.quarter(380, 420);
        proposal.resolved_price = rng_.percent(3) ? -1.0 : proposal.raw_price;
        proposal.pre_fill = position();
        proposal.pre_target = target_for(&live);
        double cap = 3.0;
        if (const auto* units = std::get_if<no::RemainingUnits>(&live.remaining)) cap = units->q;
        if (const auto* allowance = std::get_if<no::AllowanceUnits>(&live.allowance)) {
            cap = std::min(cap, allowance->left);
        }
        const double filled = cap <= 0.0 ? 1.0 : (rng_.percent(70) ? cap : cap * 0.5);
        const auto& intent = live.request().intent;
        double closed = 0.0;
        double opened = 0.0;
        if (std::holds_alternative<no::Flatten>(intent)) {
            proposal.physical_action = pineforge::execution::Flatten{};
            closed = static_cast<double>(rng_.between(1, 3));
        } else if (const auto* transact = std::get_if<no::Transact>(&intent)) {
            const double sign = transact->signed_units > 0.0 ? 1.0 : -1.0;
            proposal.physical_action = pineforge::order_action::Transact{sign * filled};
            closed = rng_.percent(40) ? filled * 0.5 : 0.0;
            opened = sign * (filled - closed);
        } else if (const auto* reverse = std::get_if<no::ReverseTo>(&intent)) {
            proposal.physical_action = pineforge::execution::ReverseTo{reverse->signed_units};
            opened = reverse->signed_units;
            closed = static_cast<double>(rng_.between(0, 2));
        } else {
            proposal.physical_action = pineforge::order_action::Reduce{filled};
            closed = filled;
        }
        if (const auto* opening = std::get_if<no::OpeningClose>(&live.authority)) {
            proposal.scope = pineforge::execution::OpeningExposure{opening->opening.incarnation,
                                                                   opening->cycle};
        } else if (const auto* closes = std::get_if<no::OpeningsClose>(&live.authority)) {
            no::SelectedExposure selected;
            selected.cycle = closes->cycle;
            for (const auto& observation : proposal.pre_target.openings) {
                if (observation.has_live_matching_lot) {
                    selected.incarnations.push_back(observation.queried_opening.incarnation);
                }
            }
            proposal.scope = selected;
        }
        proposal.inspected_closed_units = closed;
        proposal.inspected_opened_units = opened;
        proposal.inspected_current_ticket = rng_.percent(50) ? 0.0 : 0.25;
        const std::uint64_t ordinal = next_ordinal();
        no::CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.closed_units = closed;
        facts.result.opened_units = opened;
        facts.result.current_ticket = proposal.inspected_current_ticket;
        facts.result.first_trade_index = static_cast<std::size_t>(rows_);
        facts.result.closed_trade_count = closed > 0.0 ? 1 : 0;
        facts.result.opened_lot_incarnation = opened != 0.0 ? target.incarnation : 0;
        facts.cycle_before = cycle_;
        const std::int64_t cycle_after = opened != 0.0 ? cycle_ + 1 : (rng_.percent(50) ? 0 : cycle_);
        facts.cycle_after = cycle_after;
        facts.post_target = target_for(&live);
        facts.committed_action = proposal.physical_action;
        ++totals_.executions;
        step("execution",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 auto prepared = core.prepare_execution(target, proposal, o);
                 if (auto* token = std::get_if<no::PreparedExecution>(&prepared)) {
                     const auto result = core.install_execution(std::move(*token), facts);
                     if (const auto* error = std::get_if<no::InstallError>(&result)) return answer(*error);
                     return installed(std::get<no::Installed>(result).events);
                 }
                 if (const auto* none = std::get_if<no::NoChange>(&prepared)) return answer(*none);
                 return answer(std::get<no::PreparationError>(prepared));
             },
             [&](no::WorkingRequestCore& core) {
                 // The direct form's two halves around the settlement.
                 std::uint64_t o = ordinal;
                 const auto checked = core.check_execution(target, proposal, o);
                 if (std::holds_alternative<std::monostate>(checked)) {
                     const auto result = core.apply_execution(target, proposal, facts, o);
                     if (const auto* error = std::get_if<no::InstallError>(&result)) return answer(*error);
                     return installed(std::get<no::Installed>(result).events);
                 }
                 if (const auto* none = std::get_if<no::NoChange>(&checked)) return answer(*none);
                 return answer(std::get<no::PreparationError>(checked));
             });
        if (staged_.history_end() > executions_seen_
            && staged_.history_end() > staged_.history_base()
            && std::holds_alternative<no::ExecutionAppliedEvent>(
                   staged_.history_at(staged_.history_end() - 1))) {
            ++totals_.executions_installed;
        }
        executions_seen_ = staged_.history_end();
        if (closed > 0.0) ++rows_;
        if (opened != 0.0) {
            cycle_ = cycle_after;
            side_ = opened > 0.0 ? no::Side::Long : no::Side::Short;
        } else if (cycle_after == 0) {
            cycle_ = 0;
        }
    }

    no::EventId any_applied() {
        if (!applied_.empty() && rng_.percent(90)) {
            return applied_[static_cast<std::size_t>(rng_.below(static_cast<int>(applied_.size())))];
        }
        if (!all_events_.empty()) {
            return all_events_[static_cast<std::size_t>(rng_.below(static_cast<int>(all_events_.size())))];
        }
        return no::EventId{kRun, 1};
    }

    void group_effect() {
        const no::EventId applied = any_applied();
        const no::RequestHandle recipient = any_handle();
        const std::uint64_t ordinal = next_ordinal();
        step("group_effect",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_group_effect(applied, recipient, o));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_group_effect(applied, recipient, o));
             });
    }

    void owner_applied() {
        const no::EventId applied = any_applied();
        // Prefer a child that waits for the applied request.
        no::RequestHandle child = any_handle();
        const no::CommandEvent* cause = staged_.event_at(applied);
        const auto* payload = cause ? std::get_if<no::ExecutionAppliedEvent>(cause) : nullptr;
        if (payload && rng_.percent(80)) {
            for (const auto& live : staged_.live()) {
                const auto* wait = std::get_if<no::Wait>(&live.authority);
                if (wait && wait->parent == payload->handle()) {
                    child = live.handle();
                    if (rng_.percent(60)) break;
                }
            }
        }
        std::optional<no::OpeningObservation> observation;
        if (payload && rng_.percent(92)) {
            observation = observe_opening(payload->handle(), payload->cycle_after,
                                          payload->opened_units < 0.0 ? no::Side::Short
                                                                      : no::Side::Long,
                                          rng_.percent(85));
        }
        no::ArmContext arm;
        if (rng_.percent(80)) arm.price_tick = 0.25;
        const int restate = rng_.below(3);
        if (restate == 1) {
            arm.resolve_level = [](const no::LiveRequest&, const no::ExecutionAppliedEvent&,
                                   no::Side, double offset, double level) -> std::optional<double> {
                if (offset > 0.0) return level + 0.25;
                return std::nullopt;
            };
        } else if (restate == 2) {
            arm.resolve_level = [](const no::LiveRequest&, const no::ExecutionAppliedEvent&,
                                   no::Side, double, double) -> std::optional<double> {
                return -1.0;  // not a representable level
            };
        }
        const std::uint64_t ordinal = next_ordinal();
        step("owner_applied",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_owner_applied(applied, child, observation,
                                                                         o, arm));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_owner_applied(applied, child, observation, o, arm));
             });
    }

    void bound_expiry() {
        const no::EventId applied = any_applied();
        const no::RequestHandle child = any_handle();
        const no::TargetObservation observation = target_for(find(child));
        const std::uint64_t ordinal = next_ordinal();
        step("bound_expiry",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_bound_expiry(applied, child, observation, o));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_bound_expiry(applied, child, observation, o));
             });
    }

    void parent_terminal() {
        no::EventId cause = all_events_.empty()
            ? no::EventId{kRun, 1}
            : all_events_[static_cast<std::size_t>(rng_.below(static_cast<int>(all_events_.size())))];
        if (rng_.percent(5)) cause.ordinal = 0;
        const no::RequestHandle child = any_handle();
        const std::uint64_t ordinal = next_ordinal();
        step("parent_terminal",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_parent_terminal(cause, child, o));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_parent_terminal(cause, child, o));
             });
    }

    void margin_call() {
        no::MarginCallEvent event;
        if (!kernel_definitions_.empty() && rng_.percent(85)) {
            event.definition = kernel_definitions_[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(kernel_definitions_.size())))];
        } else if (!staged_.live().empty()) {
            event.definition = staged_.live().front().definition;
        }
        event.applied = any_applied();
        event.cursor = cursor();
        event.side = side();
        event.mark = rng_.quarter(380, 420);
        event.equity = 1000.0;
        event.required = 500.0;
        event.liquidation_price = event.mark - 10.0;
        event.units = 1.0;
        event.position_before = 2.0;
        event.position_after = 1.0;
        const std::uint64_t ordinal = next_ordinal();
        step("margin_call",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_margin_call(event, o));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_margin_call(event, o));
             });
    }

    void risk_event() {
        no::NativeRiskEvent event;
        event.kind = static_cast<no::RiskLimitKind>(rng_.below(4));
        event.limit = 100.0;
        event.observed = 120.0;
        event.day_ordinal = rng_.between(1, 5);
        event.cursor = cursor();
        const std::uint64_t ordinal = next_ordinal();
        step("risk_event",
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return staged_mutation(core, core.prepare_risk_event(event, o));
             },
             [&](no::WorkingRequestCore& core) {
                 std::uint64_t o = ordinal;
                 return direct_mutation(core.apply_risk_event(event, o));
             });
    }

    void retire() {
        const std::uint64_t last = staged_.last_ordinal();
        const std::uint64_t through = last == 0 ? 0
            : static_cast<std::uint64_t>(rng_.between(0, static_cast<int>(last)));
        step("retire_history",
             [&](no::WorkingRequestCore& core) {
                 return Answer{0, core.retire_history(through), {}};
             },
             [&](no::WorkingRequestCore& core) {
                 return Answer{0, core.retire_history(through), {}};
             });
        totals_.retired = static_cast<long>(staged_.history_base());
        // Ids the window retired stay in the lists: a step that names one reads
        // an event that is gone, on both cores alike.
        seen_ = std::max(seen_, staged_.history_base());
    }

    void cohort_step() {
        const int pick = rng_.below(3);
        if (pick == 0 || staged_.cohorts().empty()) {
            step("cohort_open",
                 [&](no::WorkingRequestCore& core) { return Answer{0, core.cohort_open().value, {}}; },
                 [&](no::WorkingRequestCore& core) { return Answer{0, core.cohort_open().value, {}}; });
            return;
        }
        const no::CohortHandle cohort = staged_.cohorts()[static_cast<std::size_t>(
                rng_.below(static_cast<int>(staged_.cohorts().size())))].handle;
        const no::RequestHandle origin = !openings_.empty() && rng_.percent(70)
            ? openings_[static_cast<std::size_t>(rng_.below(static_cast<int>(openings_.size())))].handle
            : any_handle();
        const bool add = pick == 1;
        step(add ? "cohort_add" : "cohort_remove",
             [&](no::WorkingRequestCore& core) {
                 if (add) core.cohort_add(cohort, origin); else core.cohort_remove(cohort, origin);
                 return Answer{0, 0, {}};
             },
             [&](no::WorkingRequestCore& core) {
                 if (add) core.cohort_add(cohort, origin); else core.cohort_remove(cohort, origin);
                 return Answer{0, 0, {}};
             });
    }

    Rng rng_;
    std::uint64_t seed_;
    no::WorkingRequestCore staged_;
    no::WorkingRequestCore direct_;
    full_fold::CoreTracker staged_tracker_;
    full_fold::CoreTracker direct_tracker_;
    Totals& totals_;
    int step_index_ = 0;
    std::uint64_t floor_ = 0;
    std::uint64_t point_ordinal_ = 0;
    std::int64_t time_ms_ = 1736121600000LL;
    std::int64_t cycle_ = 0;
    no::Side side_ = no::Side::Long;
    long rows_ = 0;
    std::size_t seen_ = 0;
    std::size_t executions_seen_ = 0;
    std::vector<no::EventId> all_events_;
    std::vector<no::EventId> applied_;
    std::vector<Opening> openings_;
    std::vector<no::DefinitionRef> kernel_definitions_;
};

void run_all() {
    Totals totals;
    for (std::uint64_t seed = 1; seed <= 400 && failures == 0; ++seed) {
        Script script(seed, totals);
        script.run(400);
    }
    std::printf("core twins: 400 streams, %ld steps (%ld installed, %ld unchanged, %ld refused, "
                "%ld thrown), %ld executions (%ld booked), %ld stale-token probes, %ld events; "
                "staged == direct\n",
                totals.steps, totals.installed, totals.no_change, totals.refused, totals.thrown,
                totals.executions, totals.executions_installed, totals.stale_probes, totals.events);
}

}  // namespace core_twins

// ===========================================================================
// Part B: whole books on the consumer, the direct forms on and off.
// ===========================================================================

// What one run of a traced host produced, beyond the fixture's own outcome:
// after every command and at every bar and fill, the full fold of the request
// core, the continuation and the broker hash; at the end, every event of the
// record field by field.
struct Trace {
    full_fold::CoreTracker tracker;
    std::vector<std::uint64_t> core;
    std::vector<std::uint64_t> continuation;
    std::vector<std::uint64_t> broker;
    std::uint64_t events = 0;
    std::size_t event_count = 0;
    std::uint64_t final_core = 0;
};

template <class Host>
void snapshot(Host& host, Trace& trace) {
    const auto& consumer = as_native_consumer(host.execution_consumer_for_test());
    trace.core.push_back(trace.tracker.step(consumer.request_core()));
    trace.continuation.push_back(host.native_continuation_hash());
    trace.broker.push_back(host.broker_state_hash());
}

// How many events of each kind the Full runs recorded, over the whole row:
// the evidence that the books reach every path the direct forms take.
std::vector<long> census(std::variant_size_v<no::CommandEvent>, 0);
long cancelled_by_reason[5] = {0, 0, 0, 0, 0};

template <class Host>
void finish_trace(Host& host, Trace& trace, bool count = false) {
    const auto& consumer = as_native_consumer(host.execution_consumer_for_test());
    trace.final_core = full_fold::core_value(consumer.request_core());
    full_fold::Fold f;
    const auto events = host.native_events(0);
    trace.event_count = events.size();
    for (const auto& row : events) {
        f.e(row.kind);
        f.u(row.ordinal);
        if (row.command) {
            full_fold::fold(f, *row.command);
            if (count) {
                ++census[row.command->index()];
                if (const auto* cancelled = std::get_if<no::CancelledEvent>(&*row.command)) {
                    ++cancelled_by_reason[static_cast<int>(cancelled->reason) % 5];
                }
            }
        }
    }
    trace.events = f.h;
}

void print_census() {
    static const char* names[] = {"Accepted", "Rejected", "Replaced", "ReplaceRejected",
        "Cancelled", "NotWorking", "InvalidHandle", "NoEffect", "MatchRejected",
        "ExecutionApplied", "CloseBound", "Activated", "ReservationReduced",
        "DeferredGroupAdjustment", "QuantityBound", "Armed", "TermsResolved", "MarginCall",
        "NativeRisk"};
    std::printf("event census of the Full runs (direct):");
    for (std::size_t kind = 0; kind < census.size(); ++kind) {
        std::printf(" %s=%ld", names[kind], census[kind]);
    }
    std::printf("; cancelled by reason User=%ld Group=%ld OwnerGone=%ld UnsupportedRelation=%ld "
                "Superseded=%ld\n", cancelled_by_reason[0], cancelled_by_reason[1],
                cancelled_by_reason[2], cancelled_by_reason[3], cancelled_by_reason[4]);
}

bool same_trace(const Trace& a, const Trace& b) {
    return a.core == b.core && a.continuation == b.continuation && a.broker == b.broker
        && a.events == b.events && a.event_count == b.event_count && a.final_core == b.final_core;
}

// ---- PERF-K3's randomized book, traced command by command ------------------
class TracedBookHost final : public k3_book::BookHost {
public:
    TracedBookHost(const k3_book::BookConfig& config, bool direct, bool window)
        : BookHost(config), window_(window) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }
    Trace trace;
    const IExecutionConsumer& execution_consumer_for_test() const { return execution_consumer(); }
    bool window_;

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        snapshot(*this, trace);
        BookHost::on_native_bar(bar, context);
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        snapshot(*this, trace);
        BookHost::on_native_applied(event, context);
    }

protected:
    void after_command() override { snapshot(*this, trace); }
};

struct BookTotals {
    long runs = 0;
    long commands = 0;
    long trades = 0;
    long events = 0;
    long applied = 0;
};

void compare_book(const k3_book::BookConfig& config, bool window, BookTotals& totals) {
    auto run = [&](bool direct) {
        auto host = std::make_unique<TracedBookHost>(config, direct, window);
        if (window) {
            // The window variant: the fixture's Full spec with the default
            // retention put back, so the journal retires at every bar.
            const k3_book::Tape tape = k3_book::make_tape(config);
            auto spec = k3_book::make_spec(config, tape);
            spec.event_retention = NativeEventRetention::Window;
            if (host->configure_native(spec).status != NativeSetupStatus::Applied) {
                host->outcome.error = "configure refused";
                return host;
            }
            host->run(tape.bars.data(), static_cast<int>(tape.bars.size()));
            host->finish();
        } else {
            k3_book::run_book(*host, config);
        }
        finish_trace(*host, host->trace, direct && !window);
        return host;
    };
    const auto direct = run(true);
    const auto staged = run(false);
    const bool traces = same_trace(direct->trace, staged->trace);
    CHECK(traces);
    CHECK(direct->outcome.completed == staged->outcome.completed);
    CHECK(direct->outcome.error == staged->outcome.error);
    CHECK(direct->outcome.trace == staged->outcome.trace);
    CHECK(direct->outcome.trades_digest == staged->outcome.trades_digest);
    CHECK(direct->outcome.events_digest == staged->outcome.events_digest);
    CHECK(direct->outcome.position == staged->outcome.position);
    CHECK(direct->outcome.completed);
    if (!traces) {
        std::fprintf(stderr, "  book seed=%llu live=%d path=%s calc=%d quantize=%d window=%d "
                     "snapshots %zu/%zu\n",
                     static_cast<unsigned long long>(config.seed), config.live,
                     k3_book::path_name(config.path), config.calc_on_fills ? 1 : 0,
                     config.quantize ? 1 : 0, window ? 1 : 0, direct->trace.core.size(),
                     staged->trace.core.size());
    }
    ++totals.runs;
    totals.commands += static_cast<long>(direct->trace.core.size());
    totals.trades += direct->outcome.trades;
    totals.events += static_cast<long>(direct->trace.event_count);
    totals.applied += direct->outcome.applied;
}

void run_books() {
    BookTotals totals;
    const k3_book::Path paths[] = {k3_book::Path::None, k3_book::Path::Synthesized,
                                   k3_book::Path::Lower};
    std::uint64_t seed = 101;
    for (int live : {2, 6, 12, 40}) {
        for (auto path : paths) {
            for (int variant = 0; variant < 4; ++variant) {
                k3_book::BookConfig config;
                config.seed = seed++;
                config.live = live;
                config.bars = 90;
                config.path = path;
                config.calc_on_fills = (variant & 1) != 0;
                config.quantize = (variant & 2) != 0;
                compare_book(config, false, totals);
                compare_book(config, true, totals);
            }
        }
    }
    std::printf("books: %ld runs, %ld command/bar/fill snapshots, %ld fills, %ld trades, %ld "
                "events; direct == staged at every snapshot\n",
                totals.runs, totals.commands, totals.applied, totals.trades, totals.events);
}

// ---- PERF-L2's fused-settlement host ---------------------------------------
class TracedFusedHost final : public l2_fused::FusedHost {
public:
    TracedFusedHost(const l2_fused::Config& config, bool direct) : FusedHost(config) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }
    Trace trace;
    const IExecutionConsumer& execution_consumer_for_test() const { return execution_consumer(); }

    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        snapshot(*this, trace);
        FusedHost::on_native_bar(bar, context);
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        snapshot(*this, trace);
        FusedHost::on_native_applied(event, context);
    }
};

void run_fused() {
    long runs = 0, fills = 0, snapshots = 0, events = 0;
    const l2_fused::Path paths[] = {l2_fused::Path::None, l2_fused::Path::Synthesized,
                                    l2_fused::Path::Lower};
    const NativeFeeKind fees[] = {NativeFeeKind::Percent, NativeFeeKind::CashPerUnit,
                                  NativeFeeKind::CashPerExecution};
    const l2_fused::Fx fxs[] = {l2_fused::Fx::Unit, l2_fused::Fx::Constant, l2_fused::Fx::Curve};
    std::uint64_t seed = 7001;
    for (int index = 0; index < 36; ++index) {
        l2_fused::Config config;
        config.seed = seed++;
        config.path = paths[index % 3];
        config.fee_kind = fees[(index / 3) % 3];
        config.fx = fxs[(index / 9) % 3];
        config.calc_on_fills = (index % 4) == 1;
        config.quantize = (index % 5) == 2;
        config.margin = (index % 3) == 0;
        config.drive = (index % 6) == 5 ? l2_fused::Drive::Stream : l2_fused::Drive::Batch;
        config.owns_excursions = (index % 7) == 3;
        config.flatten_liquidations = (index % 8) == 5;
        TracedFusedHost direct(config, true);
        TracedFusedHost staged(config, false);
        const auto a = l2_fused::run(direct, config);
        const auto b = l2_fused::run(staged, config);
        finish_trace(direct, direct.trace, true);
        finish_trace(staged, staged.trace);
        const bool traces = same_trace(direct.trace, staged.trace);
        CHECK(traces);
        CHECK(a.completed == b.completed);
        CHECK(a.error == b.error);
        CHECK(a.trace == b.trace);
        CHECK(a.precommits == b.precommits);
        CHECK(a.excursions == b.excursions);
        CHECK(a.trades_digest == b.trades_digest);
        CHECK(a.lots_digest == b.lots_digest);
        CHECK(a.events_digest == b.events_digest);
        CHECK(a.stream_hash == b.stream_hash);
        CHECK(a.broker == b.broker);
        CHECK(a.equity == b.equity);
        bool fills_equal = a.fills.size() == b.fills.size();
        for (std::size_t i = 0; fills_equal && i < a.fills.size(); ++i) {
            fills_equal = a.fills[i].digest == b.fills[i].digest
                && a.fills[i].continuation == b.fills[i].continuation
                && a.fills[i].broker == b.fills[i].broker;
        }
        CHECK(fills_equal);
        if (!traces || !fills_equal) {
            std::fprintf(stderr, "  fused seed=%llu index=%d\n",
                         static_cast<unsigned long long>(config.seed), index);
        }
        ++runs;
        fills += static_cast<long>(a.fills.size());
        snapshots += static_cast<long>(direct.trace.core.size());
        events += static_cast<long>(direct.trace.event_count);
    }
    std::printf("fused-settlement host: %ld runs, %ld fills, %ld bar/fill snapshots, %ld events; "
                "direct == staged\n", runs, fills, snapshots, events);
}

// ---- this row's scripted host ----------------------------------------------
// What neither fixture issues: anchored legs whose host restates the level,
// deferred group reductions, retaining replaces, legs pending until armed,
// acceptance-frozen kernel sizing, bound openings, the risk block's flatten
// and executions against the current price.
struct ScriptConfig {
    std::uint64_t seed = 1;
    bool window = true;
    bool calc_on_fills = false;
    bool synthesized = false;
    bool risk = false;
    bool margin = false;
    int bars = 60;
};

class ScriptHost final : public NativeStrategyHost {
public:
    ScriptHost(const ScriptConfig& config, bool direct) : config_(config), rng_(config.seed) {
        as_native_consumer(execution_consumer()).set_direct_mutation(direct);
    }
    Trace trace;
    std::vector<std::uint64_t> answers;
    long commands = 0;
    const IExecutionConsumer& execution_consumer_for_test() const { return execution_consumer(); }

    void on_native_run_begin() override {
        rng_ = core_twins::Rng(config_.seed);
        handles_.clear();
        openings_.clear();
        cohort_.reset();
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        snapshot(*this, trace);
        ref_ = static_cast<long>(bar.close * 4.0);
        refresh_handles();
        if (!cohort_ && rng_.percent(50)) {
            cohort_ = cohort_open();
            done();
        }
        const int actions = rng_.between(1, 4);
        for (int i = 0; i < actions; ++i) act();
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        snapshot(*this, trace);
        ref_ = static_cast<long>(event.resolved_price * 4.0);
        if (event.opened_units != 0.0) openings_.push_back({event.handle(), event.cycle_after});
        if (cohort_ && event.opened_units != 0.0 && rng_.percent(40)) {
            cohort_add(*cohort_, event.handle());
            done();
        }
        if (rng_.percent(30)) act();
        // An execution against the current price, inside the fill callback.
        if (rng_.percent(10)) current();
    }

    std::optional<double> resolve_anchored_level(const NativeAnchoredLevelView& view) const override {
        // A restating host: every third leg one tick further out, the rest the
        // kernel's own level.
        if (view.leg.incarnation % 3 == 0) {
            return view.kernel_level + (view.leg_side == no::Side::Long ? -0.25 : 0.25);
        }
        return std::nullopt;
    }

    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            if (facts.scope_exposure_units > 0.0) {
                terms.units = facts.scope_exposure_units < 2.0 ? facts.scope_exposure_units : 2.0;
            } else {
                terms.units = 1.0;
            }
        }
        return terms;
    }

private:
    struct Opening {
        no::RequestHandle handle;
        std::int64_t cycle = 0;
    };

    void done() {
        ++commands;
        snapshot(*this, trace);
    }

    void refresh_handles() {
        handles_.clear();
        for (const auto& row : native_working_requests()) handles_.push_back(row.definition->handle);
    }

    double px(long quarter_ticks) const { return static_cast<double>(quarter_ticks) * 0.25; }

    void note(const no::SubmitResult& result) {
        answers.push_back(core_twins::result_value(result));
        if (result.handle) handles_.push_back(*result.handle);
        done();
    }

    void act() {
        const int pick = rng_.below(100);
        const bool buy = rng_.percent(50);
        // A margin run mostly lets its position ride into the kernel's
        // liquidation instead of trading it away.
        if (config_.margin && physical_position().signed_units != 0.0 && rng_.percent(75)) return;
        if (config_.margin && physical_position().signed_units == 0.0 && rng_.percent(60)) {
            // An opening at the edge of the margin model: the first adverse
            // print breaches maintenance, so the kernel rests, re-prices and
            // withdraws its own liquidation and books its margin receipt.
            no::Sized sized;
            sized.side = buy ? no::Side::Long : no::Side::Short;
            sized.basis = no::CashValue{1990.0};
            no::Request edge{sized, "edge", ""};
            note(submit(edge));
            return;
        }
        if (rng_.percent(2)) {
            // A command naming a handle of another run.
            const no::RequestHandle stranger{no::RunIdentity{"another-run", 9}, 3};
            if (rng_.percent(50)) {
                answers.push_back(core_twins::result_value(cancel(stranger)));
            } else {
                answers.push_back(core_twins::result_value(
                        replace(stranger, no::Request{no::Transact{1.0}, "x", ""})));
            }
            done();
            return;
        }
        if (pick < 25) {
            // An entry and two legs anchored to its fill, in one OCA group whose
            // effect is Reduce half the time (a deferred reduction on a leg
            // that has no size yet), pending until armed or working, armed at
            // or after the print, on the owner's lot or the book.
            no::Request entry{no::Transact{buy ? 2.0 : -2.0}, "e", ""};
            if (rng_.percent(40)) entry.trigger = no::Limit{px(buy ? ref_ - 2 : ref_ + 2)};
            const auto parent = submit(entry);
            note(parent);
            if (!parent.handle) return;
            no::WaitForApplied wait;
            wait.parent = *parent.handle;
            wait.visibility = rng_.percent(40) ? no::NativeArmVisibility::PendingUntilArmed
                                               : no::NativeArmVisibility::Working;
            wait.first_match = rng_.percent(40) ? no::NativeArmFirstMatch::AfterArmPrint
                                                : no::NativeArmFirstMatch::AtArmPrint;
            wait.scope = rng_.percent(30) ? no::NativeArmScope::Book : no::NativeArmScope::OwnerLot;
            no::Member member;
            member.group = static_cast<std::uint64_t>(rng_.between(1, 1000000));
            member.cohort = 0;
            member.effect = rng_.percent(50) ? no::GroupEffect::Reduce : no::GroupEffect::Cancel;
            no::Request take{no::Reduce{no::OwnerOpenedUnits{}}, "tp", ""};
            take.owner = wait;
            take.group = member;
            no::Request stop{no::Reduce{no::OwnerOpenedUnits{}}, "sl", ""};
            stop.owner = wait;
            stop.group = member;
            if (rng_.percent(70)) {
                take.trigger = no::Limit{0.0};
                take.anchor = no::FromOwnerFill{buy ? 3.0 : -3.0, rng_.percent(30),
                                                static_cast<no::NativeAnchorRounding>(rng_.below(3))};
                stop.trigger = no::Stop{0.0};
                stop.anchor = no::FromOwnerFill{buy ? -3.0 : 3.0, false,
                                                no::NativeAnchorRounding::Raw};
            } else {
                take.trigger = no::Limit{px(buy ? ref_ + 8 : ref_ - 8)};
                no::Trail trail;
                trail.offset = px(rng_.between(1, 6));
                stop.trigger = trail;
            }
            if (wait.scope == no::NativeArmScope::Book) {
                take.intent = no::Reduce{no::ExplicitUnits{1.0}};
                stop.intent = no::Flatten{};
            }
            // A third member of the group on the entry's own side that fills
            // first makes a Reduce effect land on the legs before their arm.
            if (member.effect == no::GroupEffect::Reduce && rng_.percent(50)) {
                no::Request sibling{no::Transact{buy ? 1.0 : -1.0}, "sib", ""};
                no::Member other = member;
                other.cohort = 1;
                sibling.group = other;
                sibling.trigger = no::Limit{px(buy ? ref_ - 1 : ref_ + 1)};
                note(submit(sibling));
            }
            note(submit(take));
            note(submit(stop));
        } else if (pick < 40 && !handles_.empty()) {
            // Replace, retaining the trigger state when the trigger kind is kept.
            const auto target = handles_[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(handles_.size())))];
            const auto* row = find(target);
            no::Request request = row ? row->definition->request : no::Request{no::Transact{1.0}, "r", ""};
            if (auto* limit = std::get_if<no::Limit>(&request.trigger)) limit->price += 0.25;
            if (auto* stopper = std::get_if<no::Stop>(&request.trigger)) stopper->price -= 0.25;
            if (auto* trail = std::get_if<no::Trail>(&request.trigger)) trail->offset += 0.25;
            no::ReplaceOptions options;
            options.retain_trigger_state = rng_.percent(50);
            const auto result = replace(target, request, options);
            answers.push_back(core_twins::result_value(result));
            done();
        } else if (pick < 50 && !handles_.empty()) {
            const auto target = handles_[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(handles_.size())))];
            answers.push_back(core_twins::result_value(cancel(target)));
            done();
        } else if (pick < 60) {
            // Kernel-sized openings, frozen at acceptance or sized at the match.
            no::Sized sized;
            sized.side = buy ? no::Side::Long : no::Side::Short;
            sized.basis = no::CashValue{800.0};
            sized.time = rng_.percent(50) ? no::SizeTime::AtAcceptance : no::SizeTime::AtMatch;
            sized.price = rng_.percent(50) ? no::SizePrice::Signal : no::SizePrice::Resolved;
            no::Request request{sized, "sz", ""};
            if (rng_.percent(50)) request.trigger = no::Limit{px(buy ? ref_ - 1 : ref_ + 1)};
            note(submit(request));
        } else if (pick < 68 && !openings_.empty()) {
            // Closes bound to one opening or to several.
            const auto& opening = openings_[static_cast<std::size_t>(
                    rng_.below(static_cast<int>(openings_.size())))];
            no::Request close{no::Reduce{no::ExplicitUnits{1.0}}, "bo", ""};
            if (rng_.percent(50)) {
                close.owner = no::BindOpening{opening.handle, opening.cycle};
            } else {
                no::BindOpenings bind;
                bind.cycle = opening.cycle;
                for (const auto& other : openings_) {
                    if (other.cycle == opening.cycle) bind.openings.push_back(other.handle);
                }
                close.owner = bind;
                if (rng_.percent(50)) {
                    no::ScopeFraction fraction;
                    fraction.fraction = 0.5;
                    fraction.basis = rng_.percent(50) ? no::ScopeBasis::AtAcceptance
                                                      : no::ScopeBasis::AtMatch;
                    close.intent = no::Reduce{fraction};
                }
            }
            if (rng_.percent(50)) close.trigger = no::Limit{px(ref_ + rng_.between(-6, 6))};
            note(submit(close));
        } else if (pick < 74 && cohort_) {
            no::Request close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "co", ""};
            close.owner = no::BindCohort{*cohort_};
            note(submit(close));
        } else if (pick < 82) {
            no::Request reverse{no::ReverseTo{buy ? 2.0 : -2.0}, "rv", ""};
            note(submit(reverse));
        } else if (pick < 90) {
            no::Request market{no::Transact{buy ? 1.0 : -1.0}, "m", ""};
            if (rng_.percent(30)) market.intent = no::Flatten{};
            note(submit(market));
        } else if (pick < 95) {
            current();
        } else {
            no::Request host_open{no::HostSized{no::HostSizedKind::Open,
                                                buy ? no::Side::Long : no::Side::Short},
                                  "ho", ""};
            note(submit(host_open));
        }
    }

    // A market request executed at the current price at once.
    void current() {
        no::Request request{no::Transact{rng_.percent(50) ? 1.0 : -1.0}, "cur", ""};
        const auto accepted = submit(request);
        note(accepted);
        if (!accepted.handle) return;
        const auto outcome = execute_current(NativeCurrentExecution{*accepted.handle,
                                                                    NativeCurrentPriceRule::AsPresented});
        full_fold::Fold f;
        f.u(outcome.index());
        answers.push_back(f.h);
        done();
    }

    const no::LiveRequest* find(const no::RequestHandle& handle) const {
        const auto& consumer = as_native_consumer(execution_consumer());
        return consumer.request_core().find_live(handle);
    }

    ScriptConfig config_;
    core_twins::Rng rng_;
    std::vector<no::RequestHandle> handles_;
    std::vector<Opening> openings_;
    std::optional<no::CohortHandle> cohort_;
    long ref_ = 400;
};

NativeRunSpec script_spec(const ScriptConfig& config, const std::vector<Bar>& minutes) {
    NativeRunSpec spec;
    spec.identity = {"l3-script", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:L3";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 100000.0;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 0.05;
    spec.event_retention = config.window ? NativeEventRetention::Window : NativeEventRetention::Full;
    if (config.calc_on_fills) spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
    if (config.synthesized) {
        IntrabarPath::synthesized path;
        path.samples = 4;
        spec.intrabar.value = path;
    } else if (!minutes.empty() && config.seed % 3 == 0) {
        IntrabarPath::lower_tf path;
        path.bars = minutes;
        path.tf = "1";
        path.samples = 4;
        spec.intrabar.value = path;
    }
    if (config.margin) {
        spec.initial_capital = 1000.0;
        NativeMarginModel model;
        model.initial_long = 0.5;
        model.initial_short = 0.5;
        model.maintenance_long = 0.5;
        model.maintenance_short = 0.5;
        model.sizing = config.seed % 2 == 0 ? NativeLiquidationSizing::Flatten
                                            : NativeLiquidationSizing::RestoreMinimum;
        spec.margin = model;
    }
    if (config.risk) {
        NativeRiskLimits risk;
        risk.max_fills_per_day = 40;
        risk.max_drawdown = NativeLossLimit{2.0, true};
        risk.action = NativeRiskAction::FlattenAndBlock;
        spec.risk = risk;
    }
    return spec;
}

void run_scripts() {
    long runs = 0, snapshots = 0, commands = 0, trades = 0, failed_runs = 0;
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        for (int shape = 0; shape < 4; ++shape) {
            ScriptConfig config;
            config.seed = seed;
            config.window = (shape & 1) == 0;
            config.calc_on_fills = shape == 2;
            config.synthesized = shape == 3;
            config.risk = seed % 4 == 1;
            config.margin = seed % 3 == 2;
            k3_book::BookConfig tape_config;
            tape_config.seed = seed * 31 + 7;
            tape_config.bars = config.bars;
            const k3_book::Tape tape = k3_book::make_tape(tape_config);
            auto run = [&](bool direct) {
                auto host = std::make_unique<ScriptHost>(config, direct);
                const auto setup = host->configure_native(script_spec(config, tape.minutes));
                CHECK(setup.status == NativeSetupStatus::Applied);
                host->run(tape.bars.data(), static_cast<int>(tape.bars.size()));
                finish_trace(*host, host->trace, direct && !config.window);
                return host;
            };
            const auto direct = run(true);
            const auto staged = run(false);
            const bool traces = same_trace(direct->trace, staged->trace);
            CHECK(traces);
            CHECK(direct->answers == staged->answers);
            CHECK(direct->last_error() == staged->last_error());
            CHECK(direct->native_state().kind == staged->native_state().kind);
            if (direct->native_state().kind != NativeLifecycleKind::Completed) {
                // A run that stops does so on both paths at the same refusal.
                // None stops today. Until lane B-ENGINE (K-ULP1) seed 8, shape
                // 2 stopped at the kernel's own: prepare_execution refused a
                // crossing Transact whose closed and opened parts summed, in
                // binary64, one ulp above the units it was sized to (a
                // fractional short met by a kernel-sized long). Such a fill is
                // now charged exactly those units, and that run completes.
                ++failed_runs;
                const auto state = direct->native_state();
                std::fprintf(stderr, "  script seed=%llu shape=%d stopped on both paths: %s "
                             "(code %u discriminator %u)\n",
                             static_cast<unsigned long long>(seed), shape,
                             direct->last_error().c_str(),
                             static_cast<unsigned>(state.failure.code),
                             static_cast<unsigned>(state.failure.discriminator));
            }
            CHECK(direct->trade_count() == staged->trade_count());
            if (!traces) {
                std::fprintf(stderr, "  script seed=%llu shape=%d snapshots %zu/%zu\n",
                             static_cast<unsigned long long>(seed), shape,
                             direct->trace.core.size(), staged->trace.core.size());
            }
            ++runs;
            snapshots += static_cast<long>(direct->trace.core.size());
            commands += direct->commands;
            trades += direct->trade_count();
        }
    }
    // Every scripted run completes, so a change that stopped one on both
    // paths alike is still a change.
    CHECK(failed_runs == 0);
    std::printf("scripted host: %ld runs (%ld stopped identically on both paths), %ld commands, "
                "%ld snapshots, %ld trades; direct == staged\n",
                runs, failed_runs, commands, snapshots, trades);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    auto part = [&](const char* name, void (*body)()) {
        if (!only.empty() && only != name) return;
        const auto start = std::chrono::steady_clock::now();
        body();
        const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
        std::printf("  (%s: %.2f s)\n", name, seconds);
        std::fflush(stdout);
    };
    part("core", core_twins::run_all);
    part("books", run_books);
    part("fused", run_fused);
    part("scripts", run_scripts);
    if (only.empty() || only != "core") print_census();
    std::printf("%d checks\n", checks);
    if (failures == 0) {
        std::printf("test_native_direct_mutation: ok\n");
        return 0;
    }
    std::printf("test_native_direct_mutation: %d of %d checks failed\n", failures, checks);
    return 1;
}
