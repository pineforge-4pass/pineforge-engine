// CR-221 O1-E2, rows E03--E05 of the whole-run O(1) inventory (E02 is
// test_native_o1_live_book.cpp, the paged id contract of E04 is
// test_native_o1_chain_pages.cpp).
//
// A live deferred group-adjustment chain pins the command journal at its head
// (retire_history stops ahead of it), so every event and every group-effect key
// committed after the head stays in the window: J grows with the chain, and the
// chain's resolution built a vector of every prior id. These fixtures reach
// exactly that population -- N group effects folded into one waiting child --
// assert it (window length, key count, chain length), and then require what the
// rows ask for:
//   E03  the journal window keeps a fixed resident part (newest events) and
//        reads the rest back from external pages; every retained event reads
//        back to the value it had when it was committed, and a page that
//        cannot be written fails the command that needed the room before it
//        changes anything (the journal then reads back as one that never
//        failed).
//   E04  resolving the chain allocates a transient that does not grow with the
//        chain: the resolution is prepared and abandoned under a heap observer,
//        and its peak is the same for a short and a very long chain.
//   E05  the group-effect keys of that pinned journal sit in an external log;
//        the total, the retained window and every key read back unchanged.
// The fixtures use the core's public API of the previous epoch only, so this
// TU compiles against it. There they reach the populations and fail on the
// resident-byte comparisons (the heap grows with N), not on a missing
// declaration. Three populations past every resident bound of the stores
// check that additional journal events and receipt keys retain no extra heap.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "native_order_full_fold.hpp"

#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace no = pineforge::native_order;
namespace ff = full_fold;

namespace {
int failures = 0;
int checks = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
        }                                                                           \
    } while (false)
// A failed REQUIRE ends the scenario: what follows reads what it established.
#define REQUIRE(x)                                                                  \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            throw std::runtime_error(#x);                                           \
        }                                                                           \
    } while (false)

// Points the scratch files' directory (TMPDIR, read when a store creates its
// file) somewhere, and puts it back when it goes out of scope.
struct TempDirectory {
    std::string saved;
    bool had = false;
    explicit TempDirectory(const char* value) {
        const char* old = std::getenv("TMPDIR");
        had = old != nullptr;
        if (had) saved = old;
        ::setenv("TMPDIR", value, 1);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (had) ::setenv("TMPDIR", saved.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
};

no::Request limit(double units, double price, const char* label = "") {
    no::Request request{no::Transact{units}, label, ""};
    request.trigger = no::Limit{price};
    return request;
}

no::EvaluationContext evaluation_at(std::uint64_t point) {
    no::EvaluationContext context;
    context.cursor.point.ordinal = point;
    context.cursor.point.effective_time_ms = 1000;
    context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

std::uint64_t ordinal_of(const no::CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

// One order core and the commands the fixtures drive it with, each the way the
// consumer (and the journal-window suite) issues it.
struct Book {
    no::RunIdentity run{"o1-e2", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t inc = 1;
    std::uint64_t ord = 1;

    no::RequestHandle submit(const no::Request& request) {
        const auto result = core.submit(request, 0, inc, ord);
        REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle);
        return *result.handle;
    }

    void install(no::Preparation<no::PreparedMutation> preparation) {
        REQUIRE(std::holds_alternative<no::PreparedMutation>(preparation));
        auto installed = core.install_mutation(
                std::get<no::PreparedMutation>(std::move(preparation)));
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ord += std::get<no::Installed>(installed).events.count;
    }

    // Fills `target` for `units` at one print: evaluate, then execute with
    // committed facts. Answers the applied event's id.
    no::EventId fill(const no::RequestHandle& target, double units,
                     const no::PositionIdentity& before, const no::PositionIdentity& after) {
        // A driver point past every ordinal the core has committed: every
        // request already accepted is born before it.
        const std::uint64_t at = ord + 1;
        no::TargetObservation observation;
        observation.current_position = before;
        auto evaluated = core.prepare_evaluation(target, evaluation_at(at), observation, ord);
        if (std::holds_alternative<no::PreparedMutation>(evaluated)) {
            install(std::move(evaluated));
        }
        no::ExecutionProposal proposal;
        proposal.cursor.point.ordinal = at;
        proposal.cursor.point.effective_time_ms = 1000;
        proposal.raw_price = 100;
        proposal.resolved_price = 100;
        proposal.physical_action = no::Transact{units};
        proposal.pre_fill = before;
        proposal.pre_target.current_position = before;
        proposal.inspected_opened_units = units;
        proposal.inspected_current_ticket = 1;
        auto prepared = core.prepare_execution(target, proposal, ord);
        REQUIRE(std::holds_alternative<no::PreparedExecution>(prepared));
        no::CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = units;
        facts.result.current_ticket = 1;
        facts.cycle_after = 1;
        facts.post_target.current_position = after;
        facts.committed_action = no::Transact{units};
        auto installed = core.install_execution(
                std::get<no::PreparedExecution>(std::move(prepared)), facts);
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ord += std::get<no::Installed>(installed).events.count;
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&core.history().back());
        REQUIRE(applied != nullptr);
        return no::EventId{run, applied->ordinal};
    }

    // One recipient's group effect, staged (prepare, then install) or direct
    // (apply): the same events at the same positions.
    void effect(const no::EventId& cause, const no::RequestHandle& recipient, bool direct) {
        if (direct) {
            auto applied = core.apply_group_effect(cause, recipient, ord);
            REQUIRE(std::holds_alternative<no::Installed>(applied));
            ord += std::get<no::Installed>(applied).events.count;
        } else {
            install(core.prepare_group_effect(cause, recipient, ord));
        }
    }
};

// The heap the recorded window still holds, and the largest it reached while
// an observer watched.
std::size_t peak_bytes = 0;
void observe_peak(std::size_t) {
    peak_bytes = std::max(peak_bytes, global_allocation::live_bytes);
}

constexpr double kParentUnits = 1000000.0;

struct ChainResult {
    std::size_t retained = 0;       // heap the core holds once the chain stands
    std::size_t prepare_peak = 0;   // transient heap of one abandoned resolution
    std::size_t window = 0;         // journal events the pinned window retains
    std::size_t keys = 0;           // group-effect keys it retains
    std::uint64_t digest = 0;       // fold of every committed event, in order
};

// N group effects, one per source fill, are folded into the child waiting on
// `parent`: its pending chain is N long, and the journal is pinned at its
// head while every later event stays in the window. Then the parent fills and
// the chain is resolved -- once prepared and abandoned, then for real.
ChainResult run_chain(std::size_t population, bool direct) {
    ChainResult result;
    // Oracles, reserved before the heap is watched so that they never count.
    std::vector<std::uint64_t> values;     // ff::value of each event, by absolute position
    std::vector<std::uint64_t> links;      // ordinal of each deferred receipt, chain order
    std::vector<std::uint64_t> causes;     // cause ordinal of each group-effect key, key order
    values.reserve(8 * population + 64);
    links.reserve(population);
    causes.reserve(population);

    global_allocation::recording = true;
    const std::size_t start = global_allocation::live_bytes;
    try {
        Book b;
        std::size_t folded = 0;
        const auto sync = [&] {
            while (folded < b.core.history_end()) {
                values.push_back(ff::value(b.core.history_at(folded)));
                ++folded;
            }
        };
        const no::PositionIdentity flat = no::PositionFlat{};
        const no::PositionIdentity long_book = no::PositionNonflat{1, no::Side::Long};

        no::Request parent_request = limit(kParentUnits, 50.0, "parent");
        const no::RequestHandle parent = b.submit(parent_request);
        no::Request child_request{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
        child_request.owner = no::WaitForApplied{parent};
        child_request.group = no::Member{7, 2, no::GroupEffect::Reduce};
        const no::RequestHandle child = b.submit(child_request);
        sync();

        double sum = 0.0;
        no::EventId last_cause;
        for (std::size_t i = 0; i != population; ++i) {
            const double delta = (i % 3 == 0 ? 0.1 : i % 3 == 1 ? 0.2 : 0.3);
            no::Request source = limit(delta, 60.0, "source");
            source.group = no::Member{7, 1, no::GroupEffect::Reduce};
            const no::RequestHandle filler = b.submit(source);
            last_cause = b.fill(filler, delta, i == 0 ? flat : long_book, long_book);
            b.effect(last_cause, child, direct);
            links.push_back(b.core.last_ordinal());
            causes.push_back(last_cause.ordinal);
            sum += delta;
            sync();
            // The consumer retires its window as it goes; the chain's head
            // pins it. Every 32nd link keeps the base's per-retirement walk of
            // the chain (quadratic there) short enough to measure.
            if (i % 32 == 0) (void) b.core.retire_history(b.core.last_ordinal());
        }

        // The population: the chain, the pinned window, the keys.
        const no::LiveRequest* waiting = b.core.find_live(child);
        REQUIRE(waiting != nullptr);
        const no::PendingDeferred pending = std::get<no::PendingDeferred>(waiting->pending);
        REQUIRE(pending.count == population);              // E04: chain length
        CHECK(pending.total == sum);
        result.window = b.core.history_end() - b.core.history_base();
        REQUIRE(result.window >= 2 * population);           // E03: the pinned window
        REQUIRE(b.core.group_effect_receipt_count() == population);   // E05: every key counted
        result.keys = b.core.group_effect_receipt_count() - b.core.group_effect_receipt_base();
        REQUIRE(result.keys + 1 >= population && result.keys <= population);
        result.retained = global_allocation::live_bytes - start;

        // E03: every retained event reads back to what it was when committed,
        // and event_at finds the chain's receipts.
        std::size_t mismatches = 0;
        for (std::size_t position = b.core.history_base(); position < b.core.history_end();
             ++position) {
            if (ff::value(b.core.history_at(position)) != values[position]) ++mismatches;
        }
        CHECK(mismatches == 0);
        for (std::size_t i = 0; i < population; i += 61) {
            const no::CommandEvent* receipt = b.core.event_at(no::EventId{b.run, links[i]});
            REQUIRE(receipt != nullptr);
            const auto* deferred = std::get_if<no::DeferredGroupAdjustmentEvent>(receipt);
            REQUIRE(deferred != nullptr);
            CHECK(deferred->ordinal == links[i] && deferred->recipient == child);
        }
        const no::CommandEvent* newest = b.core.event_at(no::EventId{b.run, links.back()});
        REQUIRE(newest != nullptr && ordinal_of(*newest) == links.back());
        CHECK(b.core.history_at(b.core.history_end() - 1).index() == newest->index());
        // The journal's window iterates in order, ordinals ascending.
        std::uint64_t previous = 0;
        std::size_t walked = 0;
        for (const auto& event : b.core.history()) {
            const std::uint64_t ordinal = ordinal_of(event);
            if (ordinal <= previous) ++mismatches;
            previous = ordinal;
            ++walked;
        }
        CHECK(mismatches == 0 && walked == result.window);

        // E05: every retained key reads back, in key order.
        for (std::size_t index = b.core.group_effect_receipt_base();
             index < b.core.group_effect_receipt_count(); ++index) {
            const auto key = b.core.group_effect_receipt(index);
            if (!(key.recipient == child && key.effect == no::GroupEffect::Reduce
                  && key.cause.ordinal == causes[index] && key.outcome_ordinal == links[index]
                  && key.cause.run == b.run)) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        // A replayed cause finds its key.
        const auto replay = b.core.prepare_group_effect(last_cause, child, b.ord);
        REQUIRE(std::holds_alternative<no::NoChange>(replay));
        CHECK(std::get<no::NoChange>(replay).reason == no::NoChangeReason::AlreadyApplied);

        // Resolution. The parent fills; its fill arms the child and spends the
        // chain.
        const no::EventId parent_fill = b.fill(parent, kParentUnits, long_book, long_book);
        sync();
        no::OpeningObservation opening;
        opening.queried_opening = parent;
        opening.queried_cycle = 1;
        opening.current_position = long_book;
        opening.has_live_matching_lot = true;
        const std::size_t before_end = b.core.history_end();
        const std::size_t keys_before = b.core.group_effect_receipt_count();
        peak_bytes = global_allocation::live_bytes;
        const std::size_t baseline = peak_bytes;
        global_allocation::on_recorded = observe_peak;
        {
            // E04 and the rollback row: prepare the whole resolution, abandon it.
            auto abandoned = b.core.prepare_owner_applied(parent_fill, child, opening, b.ord);
            REQUIRE(std::holds_alternative<no::PreparedMutation>(abandoned));
        }
        global_allocation::on_recorded = nullptr;
        result.prepare_peak = peak_bytes - baseline;
        CHECK(b.core.history_end() == before_end);          // no partial journal row
        CHECK(b.core.group_effect_receipt_count() == keys_before);
        const no::LiveRequest* still_waiting = b.core.find_live(child);
        REQUIRE(still_waiting != nullptr);
        CHECK(std::get<no::PendingDeferred>(still_waiting->pending).count == population);

        if (direct) {
            auto resolved = b.core.apply_owner_applied(parent_fill, child, opening, b.ord);
            REQUIRE(std::holds_alternative<no::Installed>(resolved));
            b.ord += std::get<no::Installed>(resolved).events.count;
        } else {
            b.install(b.core.prepare_owner_applied(parent_fill, child, opening, b.ord));
        }
        sync();
        const auto* bound = std::get_if<no::QuantityBoundEvent>(&b.core.history_at(before_end));
        REQUIRE(bound != nullptr);
        CHECK(bound->source_units == kParentUnits);
        CHECK(bound->pending_total == sum);
        CHECK(bound->effective_deduction == sum);
        CHECK(std::get<no::RemainingProjectionUnits>(bound->remaining).q == kParentUnits - sum);
        const no::LiveRequest* armed = b.core.find_live(child);
        REQUIRE(armed != nullptr);
        CHECK(std::holds_alternative<no::PendingNone>(armed->pending));
        CHECK(std::get<no::RemainingUnits>(armed->remaining).q == kParentUnits - sum);
        // The chain is gone, so the pinned prefix retires.
        const std::size_t freed = b.core.retire_history(b.core.last_ordinal());
        CHECK(freed >= population);
        sync();
        ff::Fold digest;
        for (const std::uint64_t value : values) digest.u(value);
        result.digest = digest.h;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "o1-e2 chain fixture (N=%zu, %s): %s\n", population,
                     direct ? "direct" : "staged", error.what());
        ++failures;
    }
    global_allocation::on_recorded = nullptr;
    global_allocation::recording = false;
    return result;
}

// Every kind of command the core commits that a plain book can produce, over a
// journal long enough that most of it is external, and nothing retired: the
// pages must hold each of them exactly.
void kinds_round_trip() {
    std::vector<std::uint64_t> values;
    values.reserve(40000);
    Book b;
    std::size_t folded = 0;
    const auto sync = [&] {
        while (folded < b.core.history_end()) {
            values.push_back(ff::value(b.core.history_at(folded)));
            ++folded;
        }
    };
    auto kernel = std::make_shared<no::RequestDefinition>();
    kernel->handle = no::RequestHandle{b.run, 900000};
    kernel->request = limit(1.0, 10.0, "kernel");
    kernel->origin = no::RequestOrigin::KernelLiquidation;
    const no::RequestHandle stranger{no::RunIdentity{"o1-e2-other", 9}, 5};
    const no::PositionIdentity long_book = no::PositionNonflat{1, no::Side::Long};
    // Long enough that most of the journal is outside the heap whatever the
    // resident bound of the pages is (a few tens of thousands of events).
    constexpr std::size_t kFarWindow = 24000;
    for (std::size_t i = 0; b.core.history_end() < kFarWindow; ++i) {
        const double offset = static_cast<double>(i % 7);
        no::RequestHandle a = b.submit(limit(1.0, 90.0 - offset, "a"));
        const no::RequestHandle c = b.submit(limit(2.0, 80.0, "c"));
        // Rejected: no units.
        no::Request bad{no::Transact{0.0}, "bad", ""};
        const auto rejected = b.core.submit(bad, 0, b.inc, b.ord);
        CHECK(rejected.status == no::SubmitStatus::Rejected);
        // Replaced, plain and with the handle kept; and a replace refused.
        const auto plain = b.core.replace(a, limit(1.0, 85.0, "a2"), 0, b.inc, b.ord);
        CHECK(plain.status == no::ReplaceStatus::Replaced);
        a = *plain.successor;
        no::ReplaceOptions keep;
        keep.keep_handle = true;
        const auto kept = b.core.replace(c, limit(2.0, 75.0, "c2"), 0, b.inc, b.ord,
                                         std::nullopt, keep);
        CHECK(kept.status == no::ReplaceStatus::Replaced);
        const auto refused = b.core.replace(c, bad, 0, b.inc, b.ord);
        CHECK(refused.status == no::ReplaceStatus::ReplaceRejected);
        // Cancelled, cancelled again (not working), and a handle of another run.
        CHECK(b.core.cancel(a, b.ord).status == no::CancelStatus::Cancelled);
        CHECK(b.core.cancel(a, b.ord).status == no::CancelStatus::NotWorking);
        CHECK(b.core.cancel(stranger, b.ord).status == no::CancelStatus::InvalidHandle);
        // A fill, a kernel margin call and a risk event.
        (void) b.fill(c, 2.0, i == 0 ? no::PositionIdentity{no::PositionFlat{}} : long_book,
                      long_book);
        no::MarginCallEvent margin;
        margin.definition = kernel;
        margin.applied = no::EventId{b.run, 1};
        margin.mark = 99.0;
        margin.equity = 1000.0;
        margin.required = 500.0;
        margin.liquidation_price = 90.0;
        margin.units = 1.0;
        margin.position_before = 2.0;
        margin.position_after = 1.0;
        b.install(b.core.prepare_margin_call(margin, b.ord));
        no::NativeRiskEvent risk;
        risk.kind = no::RiskLimitKind::MaxFillsPerDay;
        risk.limit = 100.0;
        risk.observed = 120.0;
        risk.day_ordinal = static_cast<std::int64_t>(i);
        b.install(b.core.prepare_risk_event(risk, b.ord));
        sync();
    }
    const std::size_t window = b.core.history_end() - b.core.history_base();
    REQUIRE(window == b.core.history_end());        // nothing retired
    REQUIRE(window >= kFarWindow);                  // far past the resident bound
    std::set<std::size_t> kinds;
    std::size_t mismatches = 0;
    for (std::size_t position = 0; position < b.core.history_end(); ++position) {
        const no::CommandEvent& event = b.core.history_at(position);
        kinds.insert(event.index());
        if (ff::value(event) != values[position]) ++mismatches;
        if (position % 17 == 0) {
            const no::CommandEvent* found =
                    b.core.event_at(no::EventId{b.run, ordinal_of(event)});
            if (found == nullptr || ff::value(*found) != values[position]) ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    CHECK(kinds.size() >= 10);       // accepted, rejected, replaced, replace-rejected,
                                     // cancelled, not-working, invalid-handle, applied,
                                     // margin-call and risk events
    // The converted copy is the same window.
    const std::vector<no::CommandEvent> copy = b.core.history();
    REQUIRE(copy.size() == window);
    CHECK(ff::value(copy.front()) == values.front() && ff::value(copy.back()) == values.back());
    CHECK(ff::value(copy[window / 2]) == values[window / 2]);
}

// The group-effect keys sit in a log that closes its file once every key is
// retired and starts a new one with the next key. Two chains, one after the
// other on one core: each chain's keys (the second chain's at absolute indices
// after the first's, which the drain retired) read back at those indices, a
// replayed cause finds its key, and each drain empties the retained range.
void receipt_log_survives_a_drain() {
    Book b;
    const no::PositionIdentity long_book = no::PositionNonflat{1, no::Side::Long};
    constexpr std::size_t kLinks = 150;     // past the log's resident tail, so keys are written out
    std::vector<std::uint64_t> causes;      // cause ordinal of each key, by absolute index
    std::vector<std::uint64_t> outcomes;    // outcome ordinal of each key
    for (std::uint64_t group = 1; group <= 2; ++group) {
        const no::RequestHandle parent = b.submit(limit(1000.0, 50.0, "parent"));
        no::Request child_request{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
        child_request.owner = no::WaitForApplied{parent};
        child_request.group = no::Member{group, 2, no::GroupEffect::Reduce};
        const no::RequestHandle child = b.submit(child_request);
        no::EventId last_cause;
        for (std::size_t i = 0; i != kLinks; ++i) {
            no::Request source = limit(0.5, 60.0, "source");
            source.group = no::Member{group, 1, no::GroupEffect::Reduce};
            const no::RequestHandle filler = b.submit(source);
            last_cause = b.fill(filler, 0.5, long_book, long_book);
            b.effect(last_cause, child, false);
            causes.push_back(last_cause.ordinal);
            outcomes.push_back(b.core.last_ordinal());
        }
        REQUIRE(b.core.group_effect_receipt_count() == causes.size());
        // This chain's keys are all retained, the first chain's are not.
        const std::size_t base = b.core.group_effect_receipt_base();
        REQUIRE(base + kLinks == causes.size());
        std::size_t mismatches = 0;
        for (std::size_t index = base; index != b.core.group_effect_receipt_count(); ++index) {
            const auto key = b.core.group_effect_receipt(index);
            if (!(key.recipient == child && key.effect == no::GroupEffect::Reduce
                  && key.cause.ordinal == causes[index] && key.outcome_ordinal == outcomes[index])) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        const auto replay = b.core.prepare_group_effect(last_cause, child, b.ord);
        REQUIRE(std::holds_alternative<no::NoChange>(replay));
        CHECK(std::get<no::NoChange>(replay).reason == no::NoChangeReason::AlreadyApplied);
        // The parent fills, its fill arms the child and spends the chain; then
        // the window drains and every key goes with it.
        const no::EventId parent_fill = b.fill(parent, 1000.0, long_book, long_book);
        no::OpeningObservation opening;
        opening.queried_opening = parent;
        opening.queried_cycle = 1;
        opening.current_position = long_book;
        opening.has_live_matching_lot = true;
        b.install(b.core.prepare_owner_applied(parent_fill, child, opening, b.ord));
        CHECK(b.core.retire_history(b.core.last_ordinal()) > 0);
        CHECK(b.core.group_effect_receipt_base() == b.core.group_effect_receipt_count());
        CHECK(b.core.group_effect_receipt_count() == causes.size());
    }
}

// The commands the fixtures below drive, one per step: a request is
// submitted, and the next step cancels it. A pair is two events and the book
// never holds more than one request, so only the journal grows.
struct Stepper {
    Book b;
    std::optional<no::RequestHandle> open;
    std::vector<std::uint64_t> values;   // ff::value of each event, by absolute position
    std::size_t folded = 0;
    std::size_t done = 0;                // steps that completed

    Stepper() { values.reserve(70000); }
    void step() {
        if (!open) {
            open = b.submit(limit(1.0, 50.0, "x"));
        } else {
            CHECK(b.core.cancel(*open, b.ord).status == no::CancelStatus::Cancelled);
            open.reset();
        }
        ++done;
    }
    void sync() {
        while (folded < b.core.history_end()) {
            values.push_back(ff::value(b.core.history_at(folded)));
            ++folded;
        }
    }
};

// A journal that is retired as it grows stays inside its resident part, so a
// run whose scratch directory is unusable never notices: no page is written and
// no file is created before the first spill.
void windowed_run_never_touches_the_disk() {
    const TempDirectory broken("/pineforge-o1e2-no-such-directory");
    Stepper run;
    std::size_t widest = 0;
    bool refused = false;
    try {
        for (std::size_t i = 0; i != 40000; ++i) {
            run.step();
            widest = std::max(widest, run.b.core.history_end() - run.b.core.history_base());
            (void) run.b.core.retire_history(run.b.core.last_ordinal());
        }
    } catch (const std::system_error&) {
        refused = true;
    }
    CHECK(!refused);
    CHECK(widest <= 4);                                       // a step or two, never a page's worth
    CHECK(run.b.core.history_end() >= 40000);                 // and the run did commit its events
    CHECK(run.b.core.history_base() == run.b.core.history_end());
}

// A page that has to be written and cannot be (the scratch directory is gone)
// fails the command that needed the room, before that command changes
// anything: the journal, the counters and the last ordinal are as they were.
// The same step then succeeds once the directory is back, and the journal reads
// back event for event as a run that never failed would hold it.
void unwritable_page_fails_before_commit() {
    Stepper faulted;
    bool refused = false;
    std::size_t end_before = 0;
    std::uint64_t ord_before = 0;
    std::uint64_t inc_before = 0;
    std::uint64_t last_before = 0;
    {
        const TempDirectory broken("/pineforge-o1e2-no-such-directory");
        for (std::size_t i = 0; i != 60000; ++i) {
            end_before = faulted.b.core.history_end();
            ord_before = faulted.b.ord;
            inc_before = faulted.b.inc;
            last_before = faulted.b.core.last_ordinal();
            try {
                faulted.step();
            } catch (const std::system_error&) {
                refused = true;
                break;
            }
            faulted.sync();
        }
    }
    REQUIRE(refused);                       // a journal this long has an external part
    CHECK(faulted.b.core.history_end() == end_before);        // no partial journal row
    CHECK(faulted.b.ord == ord_before && faulted.b.inc == inc_before);
    CHECK(faulted.b.core.last_ordinal() == last_before);
    // The same step, now that the directory is back, and enough more that
    // several pages are written.
    for (std::size_t i = 0; i != 4000; ++i) {
        faulted.step();
        faulted.sync();
    }
    CHECK(faulted.b.core.history_end() >= end_before + 4000);
    // A run that never failed commits the same steps.
    Stepper twin;
    for (std::size_t i = 0; i != faulted.done; ++i) {
        twin.step();
        twin.sync();
    }
    CHECK(twin.b.core.history_end() == faulted.b.core.history_end());
    CHECK(twin.values == faulted.values);
    // And every retained event of the faulted journal reads back.
    std::size_t mismatches = 0;
    for (std::size_t position = faulted.b.core.history_base();
         position < faulted.b.core.history_end(); ++position) {
        if (ff::value(faulted.b.core.history_at(position)) != faulted.values[position]) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

// H1-05: the populations have equal phase within the 32-event chunks and the
// fixed receipt/page tails. The journal's resident chunks, four reload slots
// and spill buffers, the receipt tail, and the two live requests cancel.
// Journal pages and receipt keys beyond those bounds reside in files:
// allowed delta = 0 heap bytes/event * delta window
//               + 0 heap bytes/key * delta keys.
// The abandoned resolution uses fixed blocks and an external id segment,
// so its allowed transient delta is also 0 heap bytes/additional chain link.
void check_growth(const char* what, const ChainResult& small, const ChainResult& large) {
    constexpr std::size_t kHeapBytesPerExternalEvent = 0;
    constexpr std::size_t kHeapBytesPerExternalKey = 0;
    constexpr std::size_t kHeapBytesPerChainLink = 0;
    std::printf("%s: retained %zu -> %zu bytes, prepare peak %zu -> %zu bytes, window %zu -> %zu "
                "events, keys %zu -> %zu\n", what, small.retained, large.retained,
                small.prepare_peak, large.prepare_peak, small.window, large.window, small.keys,
                large.keys);
    REQUIRE(large.window > small.window && large.keys > small.keys);
    CHECK(large.retained <= small.retained
            + kHeapBytesPerExternalEvent * (large.window - small.window)
            + kHeapBytesPerExternalKey * (large.keys - small.keys));  // E03 + E05
    CHECK(large.prepare_peak <= small.prepare_peak
            + kHeapBytesPerChainLink * (large.keys - small.keys));   // E04
}
}  // namespace

int main() {
    try {
        // Warm every cache the first run would otherwise charge to itself (the
        // recycled prepared-token blocks, the interned session key).
        (void) run_chain(8, false);
        (void) run_chain(8, true);
        for (const bool direct : {false, true}) {
            // A short chain: a window of a few hundred events, wholly resident.
            const ChainResult short_run = run_chain(48, direct);
            CHECK(short_run.window >= 96);
            // Long chains: N past every resident bound (the pinned window is a
            // few events per link, so 8192 links are tens of thousands of events).
            const ChainResult small = run_chain(8192, direct);
            const ChainResult large = run_chain(24576, direct);
            const ChainResult largest = run_chain(65536, direct);
            check_growth(direct ? "direct" : "staged", small, large);
            check_growth(direct ? "direct-largest" : "staged-largest", large, largest);
            if (direct) {
                const ChainResult staged_large = run_chain(24576, false);
                // The staged and the direct forms commit the same events.
                CHECK(staged_large.digest == large.digest);
            }
        }
        kinds_round_trip();
        receipt_log_survives_a_drain();
        windowed_run_never_touches_the_disk();
        unwritable_page_fails_before_commit();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "o1-e2 fixtures: %s\n", error.what());
    }
    std::printf("test_native_o1_request_journal: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
