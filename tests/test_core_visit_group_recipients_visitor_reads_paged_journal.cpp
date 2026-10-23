// 1.7.0 review F-02 (argon cross-read of lane b1): visit_group_recipients
// reads the applied event's group, cohort, handle and ordinal from copies, so
// a visitor that reads the journal cannot pull the event out from under it.
//
// The journal keeps its newest JournalStore::kMaxResident (2,048) events
// resident and reads older ones back through a cache of 4 decoded pages
// (src/native_order.cpp). visit_group_recipients kept bare pointers into the
// applied event (the event, its payload and its group member) across the
// visitor, which the header allows to read the core. A visitor that reads
// events of 4 other pages reuses every cache slot, the applied event's
// decoded definition among them, and the walk then compared the next row
// against freed memory: AddressSanitizer reports a heap-use-after-free, and
// without it the next sibling can be skipped or a stranger visited.
//
// The run (the core alone, its public C++ API): two resting siblings of one
// Cancel group in cohorts 2 and 3, then a market member of cohort 1 that
// fills whole -- the applied event whose recipients are the two siblings.
// The paged run then commits more than 2,048 further events, so that event
// and every event the visitor reads sit in pages; the resident run commits a
// few hundred, so nothing is paged. In both, the visitor reads 8 events of 8
// other pages each time it is called. Both must visit the two siblings, in
// queue order, and nothing else.
//
// Previous-epoch API only, so this TU compiles against the unfixed head.
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

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
#define REQUIRE(x)                                                                  \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            throw std::runtime_error(#x);                                           \
        }                                                                           \
    } while (false)

constexpr std::size_t kResidentEvents = 2048;   // JournalStore::kMaxResident
constexpr std::size_t kPageEvents = 32;         // JournalStore::kChunk: a page's events
constexpr std::size_t kReads = 8;               // pages the visitor reads per call

struct Core {
    no::RunIdentity run{"f-02-group-recipients", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t inc = 1;
    std::uint64_t ord = 1;
    std::uint64_t point = 1;

    no::RequestHandle submit(const no::Request& request) {
        const auto result = core.submit(request, 0, inc, ord);
        REQUIRE(result.status == no::SubmitStatus::Accepted);
        REQUIRE(result.handle.has_value());
        return *result.handle;
    }

    void install(no::Preparation<no::PreparedMutation>&& preparation) {
        REQUIRE(std::holds_alternative<no::PreparedMutation>(preparation));
        const auto result = core.install_mutation(
            std::get<no::PreparedMutation>(std::move(preparation)));
        REQUIRE(std::holds_alternative<no::Installed>(result));
        ord += std::get<no::Installed>(result).events.count;
    }

    // Fills the market request `handle` whole on a flat book; answers the
    // journal position of its ExecutionAppliedEvent.
    std::size_t fill(const no::RequestHandle& handle, double units) {
        const no::PositionIdentity before = no::PositionFlat{};
        no::EvaluationContext evaluation;
        evaluation.cursor.point.ordinal = point = std::max(point, ord) + 1;
        evaluation.cursor.point.effective_time_ms = 1000;
        evaluation.cursor.point.provenance = NativePriceProvenance::ObservedPrint;
        evaluation.driver_class = no::DriverEligibilityClass::ObservedPrint;
        evaluation.existing_matching_bit = true;
        no::TargetObservation observation;
        observation.current_position = before;
        auto evaluated = core.prepare_evaluation(handle, evaluation, observation, ord);
        if (std::holds_alternative<no::PreparedMutation>(evaluated)) {
            install(std::move(evaluated));
        } else {
            REQUIRE(std::holds_alternative<no::NoChange>(evaluated));
        }
        no::ExecutionProposal proposal;
        proposal.cursor = evaluation.cursor;
        proposal.raw_price = 100;
        proposal.resolved_price = 100;
        proposal.physical_action = no::Transact{units};
        proposal.pre_fill = before;
        proposal.pre_target.current_position = before;
        proposal.inspected_opened_units = units;
        proposal.inspected_current_ticket = 1;
        auto prepared = core.prepare_execution(handle, proposal, ord);
        REQUIRE(std::holds_alternative<no::PreparedExecution>(prepared));
        no::CommittedExecutionFacts facts;
        facts.result.status = execution::Status::Applied;
        facts.result.opened_units = units;
        facts.result.current_ticket = 1;
        facts.cycle_before = 0;
        facts.cycle_after = 1;
        facts.post_target.current_position = no::PositionNonflat{1, no::Side::Long};
        facts.committed_action = no::Transact{units};
        const auto installed = core.install_execution(
            std::get<no::PreparedExecution>(std::move(prepared)), facts);
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ord += std::get<no::Installed>(installed).events.count;
        const std::size_t at = core.history_end() - 1;
        REQUIRE(std::get_if<no::ExecutionAppliedEvent>(&core.history_at(at)) != nullptr);
        return at;
    }
};

// The visitor: records the recipient, then reads kReads events of kReads
// other pages, each a page past the applied event's.
struct Visit {
    const no::WorkingRequestCore* core = nullptr;
    std::size_t applied_at = 0;
    std::vector<no::RequestHandle> seen;
    std::size_t reads = 0;

    static void read(void* context, const no::RequestHandle& handle) {
        auto& visit = *static_cast<Visit*>(context);
        visit.seen.push_back(handle);
        for (std::size_t page = 1; page <= kReads; ++page) {
            const no::CommandEvent& event =
                visit.core->history_at(visit.applied_at + page * kPageEvents);
            if (event.index() != std::variant_npos) ++visit.reads;
        }
    }
};

struct Outcome {
    std::vector<no::RequestHandle> expected;   // the two siblings, in queue order
    std::vector<no::RequestHandle> seen;       // what the reading visitor was handed
    std::vector<no::RequestHandle> quiet;      // group_recipients (reads nothing)
    std::size_t reads = 0;
    std::size_t applied_at = 0;
    std::size_t end = 0;
};

Outcome run(std::size_t later_events) {
    Core c;
    Outcome out;
    for (const std::int64_t cohort : {2, 3}) {
        no::Request sibling{no::Transact{10}, "sibling", ""};
        sibling.trigger = no::Limit{80};
        sibling.group = no::Member{1, cohort, no::GroupEffect::Cancel};
        out.expected.push_back(c.submit(sibling));
    }
    no::Request member{no::Transact{1}, "member", ""};
    member.group = no::Member{1, 1, no::GroupEffect::Cancel};
    out.applied_at = c.fill(c.submit(member), 1);
    const auto* applied =
        std::get_if<no::ExecutionAppliedEvent>(&c.core.history_at(out.applied_at));
    REQUIRE(applied != nullptr);
    const no::EventId cause{c.run, applied->ordinal};
    // Further events: resting rows the run never touches.
    for (std::size_t i = 0; i != later_events; ++i) {
        no::Request rest{no::Transact{1}, "rest", ""};
        rest.trigger = no::Limit{50};
        (void)c.submit(rest);
    }
    out.end = c.core.history_end();
    out.quiet = c.core.group_recipients(cause);
    Visit visit;
    visit.core = &c.core;
    visit.applied_at = out.applied_at;
    c.core.visit_group_recipients(cause, &visit, &Visit::read);
    out.seen = visit.seen;
    out.reads = visit.reads;
    return out;
}

void check_run(const Outcome& out, const char* name) {
    if (out.seen != out.expected) {
        std::fprintf(stderr, "%s: visited %zu recipients, expected %zu\n", name,
                     out.seen.size(), out.expected.size());
    }
    CHECK(out.expected.size() == 2);
    CHECK(out.quiet == out.expected);
    CHECK(out.seen == out.expected);
    // The visitor really read the journal: kReads events per call.
    CHECK(out.reads == kReads * out.seen.size());
}

}  // namespace

int main() {
    try {
        const Outcome paged = run(kResidentEvents + 400);
        // Self-checks that the run reaches the site: the applied event and
        // every event the visitor reads have more than kMaxResident events
        // after them, so all of them are paged.
        CHECK(paged.end - paged.applied_at > kResidentEvents);
        CHECK(paged.end - (paged.applied_at + kReads * kPageEvents) > kResidentEvents);
        check_run(paged, "paged");

        const Outcome resident = run(300);
        // The same reads, nothing paged.
        CHECK(resident.end <= kResidentEvents);
        CHECK(resident.applied_at + kReads * kPageEvents < resident.end);
        check_run(resident, "resident");

        // The recipients are the same wherever the journal holds the event.
        CHECK(paged.seen == resident.seen);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "visit_group_recipients over a paged journal: %s\n", error.what());
    }
    std::printf("test_core_visit_group_recipients_visitor_reads_paged_journal: %d checks, "
                "%d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
