// R4-D L10ae witness for ruling A40 rev 3 condition (2): the review P1 on
// WorkingRequestCore::refresh_point_allowances (the per-driver-point allowance
// fast path) against WorkingRequestCore::refresh_allowance (the checked route).
//
// Claim under test, in kernel terms only: the per-driver-point refresh may
// install a BookClose allowance in a point only where the checked refresh of
// the same point and the same observed book also installs one, and it must
// never install or retain-and-hide an allowance for a close whose bound book
// position has already died. Every scenario drives one chronological run twice
// over the identical physical fills: `fast` advances with
// refresh_point_allowances, `checked` with refresh_allowance. The two runs must
// agree on the close row after every driver point, so the fast path is a subset
// of the checked route and never a superset.
//
// Scope of the three terms the checked route applies:
//   * book_close_alive (the bound book's liveness) is the term the point path
//     owns and the term the review's failure scenario names. It is the term
//     walked chronologically below: a close bound to a book that an earlier
//     driver point of the same bar closed must not be re-allowed later in that
//     bar.
//   * the birth term's timeline part (a point may not predate the row's own
//     acceptance ordinal) and the driver-class term are re-derived by the
//     caller for every candidate before it may act at that point. The point
//     refresh holds no driver class, path phase, provenance or effective time
//     among its inputs, so it can neither observe nor violate those two terms;
//     the matrix below therefore pins only the grant agreement the point path
//     can own, over every book it may be handed.
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace no = pineforge::native_order;
namespace ex = pineforge::execution;
namespace oa = pineforge::order_action;

std::atomic<unsigned> g_allocations{0};
void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);     \
        }                                                               \
    } while (0)
#define CHECK_MSG(x, msg)                                               \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #x, msg); \
        }                                                               \
    } while (0)
#define REQUIRE(x)                                                      \
    do {                                                                \
        const bool ok_ = bool(x);                                       \
        CHECK(ok_);                                                     \
        if (!ok_) throw std::runtime_error("test prerequisite failed");  \
    } while (0)

const no::RunIdentity kRun{"l10ae-point-allowance-liveness", 1};
constexpr int64_t kCommandTimeMs = 1000;
constexpr int64_t kPointTimeMs = 2000;

// ---------------------------------------------------------------- fixtures ---

no::EvaluationContext print_ctx(uint64_t point) {
    no::EvaluationContext context;
    context.cursor.point.ordinal = point;
    context.cursor.point.effective_time_ms = kPointTimeMs;
    context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

no::TargetObservation observed(const no::PositionIdentity& position) {
    no::TargetObservation observation;
    observation.current_position = position;
    return observation;
}

no::PositionIdentity flat_book() { return no::PositionFlat{}; }
no::PositionIdentity long_book(int64_t cycle) {
    return no::PositionNonflat{cycle, no::Side::Long};
}
no::PositionIdentity short_book(int64_t cycle) {
    return no::PositionNonflat{cycle, no::Side::Short};
}
int64_t cycle_of(const no::PositionIdentity& position) {
    const auto* nonflat = std::get_if<no::PositionNonflat>(&position);
    return nonflat ? nonflat->cycle : 0;
}

// Normalises an allowance so the two routes compare value for value.
struct AllowanceView {
    int kind = 3;  // 0 units, 1 all-scope, 2 deferred, 3 unset
    uint64_t point = 0;
    double initial = 0.0;
    double left = 0.0;

    bool operator==(const AllowanceView& other) const {
        return kind == other.kind && point == other.point && initial == other.initial
            && left == other.left;
    }
};

AllowanceView view(const no::Allowance& allowance) {
    AllowanceView out;
    if (const auto* units = std::get_if<no::AllowanceUnits>(&allowance)) {
        out.kind = 0;
        out.point = units->point_ordinal;
        out.initial = units->initial;
        out.left = units->left;
    } else if (const auto* all = std::get_if<no::AllowanceAllScope>(&allowance)) {
        out.kind = 1;
        out.point = all->point_ordinal;
    } else if (const auto* deferred = std::get_if<no::AllowanceDeferred>(&allowance)) {
        out.kind = 2;
        out.point = deferred->point_ordinal;
    }
    return out;
}

std::string describe(const AllowanceView& value) {
    std::string out = value.kind == 0 ? "units" : value.kind == 1 ? "all"
                  : value.kind == 2 ? "deferred" : "unset";
    out += "{point=" + std::to_string(value.point) + ", initial=" + std::to_string(value.initial)
        + ", left=" + std::to_string(value.left) + "}";
    return out;
}

// A close proposal for the physical preparation boundary: it spends only the
// allowance a point refresh already granted at the proposal's own point.
no::ExecutionProposal close_proposal(uint64_t point, double units,
                                     const no::PositionIdentity& book) {
    no::ExecutionProposal proposal;
    proposal.cursor.point.ordinal = point;
    proposal.cursor.point.effective_time_ms = kPointTimeMs;
    proposal.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    proposal.raw_price = 110.0;
    proposal.resolved_price = 110.0;
    proposal.physical_action = oa::Reduce{units};
    proposal.scope = ex::Book{};
    proposal.pre_fill = book;
    proposal.pre_target = observed(book);
    proposal.inspected_closed_units = std::holds_alternative<no::PositionFlat>(book) ? 0.0 : units;
    proposal.inspected_opened_units = 0.0;
    proposal.inspected_current_ticket = 1;
    return proposal;
}

// One chronological run driven twice: the fast point path and the checked
// refresh, over identical submitted commands and installed physical fills.
struct Run {
    no::WorkingRequestCore core{kRun};
    uint64_t inc = 1;
    uint64_t ord = 1;
};

struct Pair {
    Run fast;
    Run checked;

    uint64_t next_point() {
        REQUIRE(fast.ord == checked.ord);
        return std::max(fast.ord, checked.ord) + 1;
    }
};

no::RequestHandle submit_on(Run& run, const no::Request& request) {
    const auto result = run.core.submit(request, kCommandTimeMs, run.inc, run.ord);
    REQUIRE(result.status == no::SubmitStatus::Accepted);
    REQUIRE(result.handle);
    return *result.handle;
}

// The same command in both runs. The shared run identity makes the handle
// valid in either core, and the two ordinals stay in lockstep.
no::RequestHandle submit_both(Pair& p, const no::Request& request) {
    const auto fast = submit_on(p.fast, request);
    const auto checked = submit_on(p.checked, request);
    REQUIRE(fast == checked);
    REQUIRE(p.fast.ord == p.checked.ord);
    return fast;
}

// The ordinary (durable) evaluation route: used for entry legs and for close
// binds. Never used for the close under test inside a driver-point walk, which
// is exactly what the fast path replaces there.
void evaluate_on(Run& run, const no::RequestHandle& handle, uint64_t point,
                 const no::PositionIdentity& position) {
    auto prepared = run.core.prepare_evaluation(handle, print_ctx(point), observed(position),
                                                run.ord);
    if (const auto* error = std::get_if<no::PreparationError>(&prepared)) {
        std::printf("  [eval] point=%llu inc=%llu code=%d\n",
                    static_cast<unsigned long long>(point),
                    static_cast<unsigned long long>(handle.incarnation),
                    static_cast<int>(error->code));
    }
    REQUIRE(!std::holds_alternative<no::PreparationError>(prepared));
    if (auto* mutation = std::get_if<no::PreparedMutation>(&prepared)) {
        const auto installed = run.core.install_mutation(std::move(*mutation));
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        run.ord += std::get<no::Installed>(installed).events.count;
    }
}

void evaluate_both(Pair& p, const no::RequestHandle& handle, uint64_t point,
                   const no::PositionIdentity& position) {
    evaluate_on(p.fast, handle, point, position);
    evaluate_on(p.checked, handle, point, position);
    REQUIRE(p.fast.ord == p.checked.ord);
}

// Installs the same committed physical fill in both runs.
void fill_on(Run& run, const no::RequestHandle& handle, uint64_t point,
             const no::ExecutionPlan& action, double closed, double opened,
             const no::PositionIdentity& before, const no::PositionIdentity& after) {
    no::ExecutionProposal proposal;
    proposal.cursor.point.ordinal = point;
    proposal.cursor.point.effective_time_ms = kPointTimeMs;
    proposal.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    proposal.raw_price = 110.0;
    proposal.resolved_price = 110.0;
    proposal.physical_action = action;
    proposal.scope = ex::Book{};
    proposal.pre_fill = before;
    proposal.pre_target = observed(before);
    proposal.inspected_closed_units = closed;
    proposal.inspected_opened_units = opened;
    proposal.inspected_current_ticket = 1;
    auto prepared = run.core.prepare_execution(handle, proposal, run.ord);
    if (const auto* error = std::get_if<no::PreparationError>(&prepared)) {
        std::printf("  [fill] point=%llu inc=%llu code=%d\n",
                    static_cast<unsigned long long>(point),
                    static_cast<unsigned long long>(handle.incarnation),
                    static_cast<int>(error->code));
    }
    REQUIRE(!std::holds_alternative<no::PreparationError>(prepared));
    if (const auto* none = std::get_if<no::NoChange>(&prepared)) {
        std::printf("  [fill-nochange] point=%llu inc=%llu reason=%d\n",
                    static_cast<unsigned long long>(point),
                    static_cast<unsigned long long>(handle.incarnation),
                    static_cast<int>(none->reason));
    }
    REQUIRE(std::holds_alternative<no::PreparedExecution>(prepared));
    no::CommittedExecutionFacts facts;
    facts.result.status = ex::Status::Applied;
    facts.result.closed_units = closed;
    facts.result.opened_units = opened;
    facts.result.current_ticket = 1;
    facts.cycle_before = cycle_of(before);
    facts.cycle_after = cycle_of(after);
    facts.post_target = observed(after);
    facts.committed_action = action;
    const auto installed
        = run.core.install_execution(std::get<no::PreparedExecution>(std::move(prepared)), facts);
    REQUIRE(std::holds_alternative<no::Installed>(installed));
    run.ord += std::get<no::Installed>(installed).events.count;
}

void fill_both(Pair& p, const no::RequestHandle& handle, uint64_t point,
               const no::ExecutionPlan& action, double closed, double opened,
               const no::PositionIdentity& before, const no::PositionIdentity& after) {
    fill_on(p.fast, handle, point, action, closed, opened, before, after);
    fill_on(p.checked, handle, point, action, closed, opened, before, after);
    REQUIRE(p.fast.ord == p.checked.ord);
}

void refresh_fast(Run& run, uint64_t point, const no::PositionIdentity& position) {
    run.core.refresh_point_allowances(point, position);
}

bool refresh_checked(Run& run, const no::RequestHandle& handle, uint64_t point,
                     const no::PositionIdentity& position) {
    return run.core.refresh_allowance(handle, print_ctx(point), observed(position));
}

AllowanceView allowance_of(const Run& run, const no::RequestHandle& handle) {
    const auto* live = run.core.find_live(handle);
    REQUIRE(live);
    return view(live->allowance);
}

// After every driver point the two runs must agree on the close row: same
// liveness, same allowance value.
void agree(const Pair& p, const no::RequestHandle& handle, const char* what) {
    const auto* fast = p.fast.core.find_live(handle);
    const auto* checked = p.checked.core.find_live(handle);
    CHECK_MSG((fast != nullptr) == (checked != nullptr), what);
    if (!fast || !checked) return;
    const auto a = view(fast->allowance);
    const auto b = view(checked->allowance);
    CHECK_MSG(a == b, (std::string(what) + " fast=" + describe(a) + " checked=" + describe(b)).c_str());
}

struct Bound {
    Pair p;
    no::RequestHandle seed;
    no::RequestHandle sibling;   // another close of the same book
    no::RequestHandle closer;    // the close under test, bound to cycle 7 Long
    no::RequestHandle reopen;    // an entry leg that can re-open the book
    no::RequestHandle flip;      // an entry leg that can open the other side
    uint64_t bind_point = 0;
    no::PositionIdentity position = flat_book();
};

// Seeds a long book at cycle 7 and binds two closes to it through the ordinary
// route, then checks both routes installed the same allowance.
void bind_close_to_cycle7(Bound& b) {
    b.seed = submit_both(b.p, no::Request{no::Transact{5.0}, "seed-entry", ""});
    b.sibling = submit_both(b.p, no::Request{no::Reduce{no::ExplicitUnits{5.0}}, "sibling-close", ""});
    b.closer = submit_both(b.p, no::Request{no::Reduce{no::ExplicitUnits{5.0}}, "bound-close", ""});
    b.reopen = submit_both(b.p, no::Request{no::Transact{5.0}, "reopen-entry", ""});
    b.flip = submit_both(b.p, no::Request{no::Transact{-4.0}, "flip-entry", ""});

    // The seed entry opens the book at cycle 7.
    const uint64_t point = b.p.next_point();
    evaluate_both(b.p, b.seed, point, b.position);
    const no::PositionIdentity opened = long_book(7);
    fill_both(b.p, b.seed, point, no::ExecutionPlan{oa::Transact{5.0}}, 0.0, 5.0, b.position,
              opened);
    b.position = opened;
    CHECK(!b.p.fast.core.find_live(b.seed));

    // Both closes bind to cycle 7 at the following driver point.
    b.bind_point = b.p.next_point();
    evaluate_both(b.p, b.sibling, b.bind_point, b.position);
    evaluate_both(b.p, b.closer, b.bind_point, b.position);
    for (auto* core : {&b.p.fast, &b.p.checked}) {
        const auto* live = core->core.find_live(b.closer);
        REQUIRE(live);
        CHECK(std::holds_alternative<no::BookClose>(live->authority));
        const auto& close = std::get<no::BookClose>(live->authority);
        CHECK(close.cycle == 7);
        CHECK(close.side == no::Side::Long);
        CHECK(view(live->allowance) == (AllowanceView{0, b.bind_point, 5.0, 5.0}));
    }
}

// ------------------------------------------------------- the pinned checks ---

// (1) The review scenario: a BookClose whose bound lot is closed by an earlier
// driver point of the same bar is not re-allowed at any later point of that
// bar. The later points sample first a flat book, then a live lot that is not
// the bound one, which is exactly what a bar that closed and re-entered hands
// the point path.
void closed_lot_is_not_re_allowed_at_a_later_point() {
    Bound b;
    bind_close_to_cycle7(b);
    uint64_t point = b.bind_point;

    // Point 1 of the bar: both closes are live and re-allowed; the sibling then
    // closes the whole book, so the lot bound to the tested close dies here.
    const uint64_t death = ++point;
    refresh_fast(b.p.fast, death, b.position);
    CHECK(refresh_checked(b.p.checked, b.sibling, death, b.position));
    CHECK(refresh_checked(b.p.checked, b.closer, death, b.position));
    agree(b.p, b.sibling, "death point sibling close");
    agree(b.p, b.closer, "death point bound close");
    CHECK(allowance_of(b.p.fast, b.closer) == (AllowanceView{0, death, 5.0, 5.0}));
    fill_both(b.p, b.sibling, death, no::ExecutionPlan{oa::Reduce{5.0}}, 5.0, 0.0, b.position,
              flat_book());
    b.position = flat_book();
    CHECK(!b.p.fast.core.find_live(b.sibling));
    // The tested close is not retired by another row's fill: it is still
    // working with the allowance its last live point installed. The whole risk
    // of the fast path is whether a later point hands it a fresh one.
    REQUIRE(b.p.fast.core.find_live(b.closer));

    // Point 2: the sampled book is flat. Neither route may re-allow the close,
    // and neither may move the allowance stamp forward off the live point.
    const uint64_t flat_point = ++point;
    refresh_fast(b.p.fast, flat_point, b.position);
    CHECK(!refresh_checked(b.p.checked, b.closer, flat_point, b.position));
    agree(b.p, b.closer, "flat point");
    const auto after_flat = allowance_of(b.p.fast, b.closer);
    CHECK_MSG(after_flat == (AllowanceView{0, death, 5.0, 5.0}), describe(after_flat).c_str());

    // The bar re-enters at the same point through a fresh entry leg, so the
    // next point samples a live book that is not the bound lot.
    const no::PositionIdentity reopened = long_book(9);
    evaluate_both(b.p, b.reopen, flat_point, b.position);
    fill_both(b.p, b.reopen, flat_point, no::ExecutionPlan{oa::Transact{5.0}}, 0.0, 5.0,
              b.position, reopened);
    b.position = reopened;

    // Point 3: a newer cycle of the bound side is not the lot that died.
    const uint64_t reopen_point = ++point;
    refresh_fast(b.p.fast, reopen_point, b.position);
    CHECK(!refresh_checked(b.p.checked, b.closer, reopen_point, b.position));
    agree(b.p, b.closer, "reopened point");
    const auto after_reopen = allowance_of(b.p.fast, b.closer);
    CHECK_MSG(after_reopen == (AllowanceView{0, death, 5.0, 5.0}), describe(after_reopen).c_str());
    // The stale allowance of the dead lot is also unspendable at this point on
    // both routes, so the fast path's refusal above is the whole difference: no
    // fill can follow an allowance that a point refresh should not have kept.
    uint64_t probe_ordinal = b.p.next_point();
    const auto stale_fast = b.p.fast.core.prepare_execution(
            b.closer, close_proposal(reopen_point, 5.0, b.position), probe_ordinal);
    const auto stale_checked = b.p.checked.core.prepare_execution(
            b.closer, close_proposal(reopen_point, 5.0, b.position), probe_ordinal);
    const bool fast_refused = std::holds_alternative<no::NoChange>(stale_fast);
    const bool checked_refused = std::holds_alternative<no::NoChange>(stale_checked);
    CHECK(fast_refused);
    CHECK_MSG(fast_refused == checked_refused, "stale allowance acts on one route only");
}

// (2) Positive control: a live lot's BookClose IS re-allowed at a later point
// of the bar, with the same value the checked route installs.
void live_lot_is_re_allowed_at_a_later_point() {
    Bound b;
    bind_close_to_cycle7(b);
    uint64_t point = b.bind_point;

    // The sibling only takes part of the book, so cycle 7 stays alive.
    const uint64_t partial = ++point;
    refresh_fast(b.p.fast, partial, b.position);
    CHECK(refresh_checked(b.p.checked, b.sibling, partial, b.position));
    CHECK(refresh_checked(b.p.checked, b.closer, partial, b.position));
    fill_both(b.p, b.sibling, partial, no::ExecutionPlan{oa::Reduce{2.0}}, 2.0, 0.0, b.position,
              long_book(7));
    CHECK(b.p.fast.core.find_live(b.sibling));

    const uint64_t later = ++point;
    refresh_fast(b.p.fast, later, long_book(7));
    CHECK(refresh_checked(b.p.checked, b.closer, later, long_book(7)));
    agree(b.p, b.closer, "live lot later point");
    const auto granted = allowance_of(b.p.fast, b.closer);
    CHECK_MSG(granted == (AllowanceView{0, later, 5.0, 5.0}), describe(granted).c_str());
    CHECK(allowance_of(b.p.checked, b.closer) == granted);
    CHECK(b.p.fast.core.find_live(b.closer));
}

// (3) Grant agreement over every book the point path can be handed at a fresh
// point of the bar: it never installs where the checked route refuses, and it
// installs the identical value where it does grant.
void point_refresh_agrees_with_the_checked_route_over_the_book_matrix() {
    Bound b;
    bind_close_to_cycle7(b);
    uint64_t point = b.bind_point;
    const no::PositionIdentity samples[] = {
        flat_book(),        // the lot closed
        long_book(7),       // the bound lot, alive
        long_book(9),       // a newer cycle of the same side
        short_book(7),      // the same cycle on the other side
        short_book(12),     // a flipped book
        long_book(13),      // re-entered again
    };
    uint64_t alive_grants = 0;
    uint64_t held = b.bind_point;
    for (const auto& sample : samples) {
        const uint64_t at = ++point;
        refresh_fast(b.p.fast, at, sample);
        const bool granted_checked = refresh_checked(b.p.checked, b.closer, at, sample);
        agree(b.p, b.closer, "book matrix point");
        const auto granted_fast = allowance_of(b.p.fast, b.closer);
        if (granted_fast.kind == 0 && granted_fast.point == at) {
            ++alive_grants;
            held = at;
            CHECK(granted_checked);
            CHECK(granted_fast == (AllowanceView{0, at, 5.0, 5.0}));
        } else {
            CHECK(!granted_checked);
            CHECK(granted_fast == (AllowanceView{0, held, 5.0, 5.0}));
        }
    }
    // The matrix is only meaningful if the live book really did re-allow.
    CHECK_MSG(alive_grants == 1, std::to_string(alive_grants).c_str());
}

// (4) The point refresh stays a no-event continuation of an allowance the row
// already holds: it may not re-initialise (and so hand back units that a fill
// of this very point already consumed) a same-point allowance, and it may not
// touch legs it does not own.
void same_point_refresh_never_hands_back_a_consumed_allowance() {
    Bound b;
    bind_close_to_cycle7(b);
    const uint64_t point = b.p.next_point();

    // The fast path is a durable-state-only refresh: no receipt, no heap.
    const auto history_before = b.p.fast.core.history().size();
    refresh_fast(b.p.fast, point, b.position);
    const auto allocations_before = g_allocations.load();
    refresh_fast(b.p.fast, point, b.position);
    CHECK(g_allocations.load() == allocations_before);
    CHECK(b.p.fast.core.history().size() == history_before);
    CHECK(refresh_checked(b.p.checked, b.closer, point, b.position));
    agree(b.p, b.closer, "consumption point");

    // Half of the bound close fills at this point; the lot survives.
    fill_both(b.p, b.closer, point, no::ExecutionPlan{oa::Reduce{2.0}}, 2.0, 0.0, b.position,
              long_book(7));
    b.position = long_book(7);
    const auto consumed = allowance_of(b.p.fast, b.closer);
    CHECK(consumed.kind == 0);
    CHECK(consumed.point == point);
    CHECK_MSG(consumed.left < consumed.initial, describe(consumed).c_str());

    // Refreshing the same point again may not hand the consumed units back.
    refresh_fast(b.p.fast, point, b.position);
    const auto held = allowance_of(b.p.fast, b.closer);
    CHECK_MSG(held == consumed, describe(held).c_str());
    CHECK(!refresh_checked(b.p.checked, b.closer, point, b.position));
    agree(b.p, b.closer, "same point refresh");

    // A leg the point path does not own keeps getting nothing from it: the
    // still-unbound entry legs never hold an allowance from a point refresh.
    const uint64_t later = point + 1;
    refresh_fast(b.p.fast, later, b.position);
    const auto* reopen = b.p.fast.core.find_live(b.reopen);
    REQUIRE(reopen);
    CHECK(view(reopen->allowance).kind == 3);
    CHECK(allowance_of(b.p.fast, b.closer).point == later);
}

// (5) The fast path neither grants to a dead lot nor retires one early: the
// durable liveness response stays available on the ordinary route at the next
// evaluation, so deferring it by a point does not lose the retirement.
void dead_close_still_retires_on_the_ordinary_route() {
    Bound b;
    bind_close_to_cycle7(b);
    uint64_t point = b.bind_point;

    const uint64_t death = ++point;
    refresh_fast(b.p.fast, death, b.position);
    CHECK(refresh_checked(b.p.checked, b.sibling, death, b.position));
    CHECK(refresh_checked(b.p.checked, b.closer, death, b.position));
    agree(b.p, b.closer, "death point");
    fill_both(b.p, b.sibling, death, no::ExecutionPlan{oa::Reduce{5.0}}, 5.0, 0.0, b.position,
              flat_book());
    b.position = flat_book();

    const uint64_t stale = ++point;
    refresh_fast(b.p.fast, stale, b.position);
    CHECK(allowance_of(b.p.fast, b.closer).point == death);
    CHECK(b.p.fast.core.find_live(b.closer));

    evaluate_on(b.p.fast, b.closer, stale, b.position);
    evaluate_on(b.p.checked, b.closer, stale, b.position);
    for (auto* run : {&b.p.fast, &b.p.checked}) {
        REQUIRE(!run->core.history().empty());
        const auto& event = run->core.history().back();
        REQUIRE(std::holds_alternative<no::CancelledEvent>(event));
        const auto& cancelled = std::get<no::CancelledEvent>(event);
        CHECK(cancelled.reason == no::CancelReason::OwnerGone);
        CHECK(cancelled.handle() == b.closer);
        CHECK(!run->core.find_live(b.closer));
    }
}
}  // namespace

int main() {
    try {
        closed_lot_is_not_re_allowed_at_a_later_point();
        live_lot_is_re_allowed_at_a_later_point();
        point_refresh_agrees_with_the_checked_route_over_the_book_matrix();
        same_point_refresh_never_hands_back_a_consumed_allowance();
        dead_close_still_retires_on_the_ordinary_route();
    } catch (const std::exception& e) {
        std::printf("FAIL unhandled exception: %s\n", e.what());
        ++failures;
    }
    std::printf("l10ae point-allowance liveness: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
