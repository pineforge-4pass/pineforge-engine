// CR-221 O1-E2, row E04: the paged contract for the prior adjustment ids a
// resolution spent.
//
// A request whose units are deferred collects the group fills deducted into it
// as a chain of receipts, and the event that resolves it (TermsResolvedEvent
// for a host-sized request, QuantityBoundEvent for an owner-opened close)
// listed every prior id in a vector: O(chain length) in the prepared token and
// in the retained event. Now a chain of up to kInlineAdjustmentIds links is
// listed in the event exactly as before, and a longer one is kept in an
// external list that visit_prior_adjustments reads back in fixed pages, in
// chain order, with the same exact pending total.
//
// The fixtures reach both populations (a chain inside the inline bound and one
// past it), both resolution events, and both command forms (staged and
// direct), and assert: the id list, its order and count, the exact total and
// deduction, the surviving request's state, that a visitor can stop, and that
// a resolution prepared and abandoned, or one whose external list cannot be
// written, leaves the chain whole for the next attempt.
//
// Against the previous epoch this TU does not compile: the paged contract
// (visit_prior_adjustments, prior_adjustment_count, kInlineAdjustmentIds) is
// what the lane adds; the bytes it saves are measured by
// test_native_o1_request_journal.cpp, which compiles against both.
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace no = pineforge::native_order;
namespace ex = pineforge::execution;

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
#define REQUIRE(x)                                                                  \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            throw std::runtime_error(#x);                                           \
        }                                                                           \
    } while (false)

no::TargetObservation flat_book() {
    no::TargetObservation target;
    target.current_position = no::PositionFlat{};
    return target;
}

no::TargetObservation nonflat_book(int64_t cycle = 1, no::Side side = no::Side::Long) {
    no::TargetObservation target;
    target.current_position = no::PositionNonflat{cycle, side};
    return target;
}

no::EvaluationContext point(std::uint64_t ordinal) {
    no::EvaluationContext context;
    context.cursor.point.ordinal = ordinal;
    context.cursor.point.effective_time_ms = 1000;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

no::TermsResolvedInput terms(double price, std::optional<double> units) {
    no::TermsResolvedInput input;
    input.price_kind = no::NativeCandidatePriceKind::PointPrice;
    input.raw_price = price;
    input.default_resolved_price = price;
    input.terms.resolved_price = price;
    input.terms.units = units;
    input.terms.shape = no::OpeningShape::Transact;
    return input;
}

no::ExecutionProposal proposal_at(const no::EvaluationContext& context, no::ExecutionPlan plan,
                                  double closed, double opened) {
    no::ExecutionProposal proposal;
    proposal.cursor = context.cursor;
    proposal.raw_price = 100.0;
    proposal.resolved_price = 100.0;
    proposal.physical_action = std::move(plan);
    proposal.scope = ex::Book{};
    proposal.pre_fill = no::PositionFlat{};
    proposal.inspected_closed_units = closed;
    proposal.inspected_opened_units = opened;
    proposal.inspected_current_ticket = 3.0;
    return proposal;
}

// The commands the fixtures drive one order core with, each the way the
// consumer (and the terms-core suite) issues it.
struct Fixture {
    no::RunIdentity run{"o1-e2-chain-pages", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;

    no::SubmitResult submit(no::Request request) {
        no::CommandContext context;
        context.decision_time_ms = 1000;
        auto prepared = core.prepare_submit(request, context, incarnation, ordinal);
        const no::SubmitResult predicted = prepared.predicted();
        auto installed = core.install_submit(std::move(prepared));
        REQUIRE(std::holds_alternative<no::CommandInstalled<no::SubmitResult>>(installed));
        ordinal += std::get<no::CommandInstalled<no::SubmitResult>>(installed).events.count;
        if (predicted.status == no::SubmitStatus::Accepted) {
            REQUIRE(predicted.handle.has_value());
            incarnation = predicted.handle->incarnation + 1;
        }
        return predicted;
    }

    no::EvaluationContext evaluate(const no::RequestHandle& target,
                                   const no::TargetObservation& observation) {
        const no::EvaluationContext context = point(ordinal);
        auto prepared = core.prepare_evaluation(target, context, observation, ordinal);
        if (auto* mutation = std::get_if<no::PreparedMutation>(&prepared)) {
            auto installed = core.install_mutation(std::move(*mutation));
            REQUIRE(std::holds_alternative<no::Installed>(installed));
            ordinal += std::get<no::Installed>(installed).events.count;
        } else {
            REQUIRE(std::holds_alternative<no::NoChange>(prepared));
        }
        return context;
    }

    void install_mutation(no::Preparation<no::PreparedMutation>&& prepared) {
        REQUIRE(std::holds_alternative<no::PreparedMutation>(prepared));
        auto installed = core.install_mutation(std::get<no::PreparedMutation>(std::move(prepared)));
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ordinal += std::get<no::Installed>(installed).events.count;
    }

    no::ExecutionAppliedEvent apply(const no::RequestHandle& target,
                                    const no::ExecutionProposal& proposal,
                                    no::TargetObservation after = nonflat_book()) {
        auto prepared = core.prepare_execution(target, proposal, ordinal);
        REQUIRE(std::holds_alternative<no::PreparedExecution>(prepared));
        no::CommittedExecutionFacts facts;
        facts.result.status = ex::Status::Applied;
        facts.result.closed_units = proposal.inspected_closed_units;
        facts.result.opened_units = proposal.inspected_opened_units;
        facts.result.current_ticket = proposal.inspected_current_ticket;
        facts.result.first_trade_index = 7;
        facts.result.closed_trade_count = proposal.inspected_closed_units == 0.0 ? 0 : 1;
        facts.cycle_before = 1;
        facts.cycle_after = std::holds_alternative<no::PositionFlat>(after.current_position) ? 0 : 2;
        facts.post_target = std::move(after);
        facts.committed_action = proposal.physical_action;
        auto installed = core.install_execution(
                std::get<no::PreparedExecution>(std::move(prepared)), facts);
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ordinal += std::get<no::Installed>(installed).events.count;
        REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(core.history().back()));
        return std::get<no::ExecutionAppliedEvent>(core.history().back());
    }

    // One group effect on `recipient`, staged or direct.
    void effect(const no::EventId& cause, const no::RequestHandle& recipient, bool direct) {
        if (direct) {
            auto applied = core.apply_group_effect(cause, recipient, ordinal);
            REQUIRE(std::holds_alternative<no::Installed>(applied));
            ordinal += std::get<no::Installed>(applied).events.count;
        } else {
            install_mutation(core.prepare_group_effect(cause, recipient, ordinal));
        }
    }
};

// The chain: `population` group fills deducted into one request whose units
// are deferred -- a host-sized open (resolved by terms) -- in commit order.
struct Chain {
    Fixture f;
    no::RequestHandle target{};
    std::vector<std::uint64_t> links;     // ordinal of each deferred receipt, chain order
    double sum = 0.0;                     // the exact total, accumulated in the same order
    no::EvaluationContext target_context;

    Chain(std::size_t population, bool direct) {
        no::Request deferred{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "deferred", ""};
        deferred.group = no::Member{44, 2, no::GroupEffect::Reduce};
        const auto placed = f.submit(deferred);
        REQUIRE(placed.handle.has_value());
        target = *placed.handle;
        links.reserve(population);
        for (std::size_t i = 0; i != population; ++i) {
            const double delta = (i % 3 == 0 ? 0.1 : i % 3 == 1 ? 0.2 : 0.3);
            no::Request source{no::Transact{delta}, "source", ""};
            source.group = no::Member{44, 1, no::GroupEffect::Reduce};
            const auto submitted = f.submit(source);
            REQUIRE(submitted.handle.has_value());
            const auto eval = f.evaluate(*submitted.handle, flat_book());
            const auto applied = f.apply(
                    *submitted.handle,
                    proposal_at(eval, no::ExecutionPlan{no::Transact{delta}}, 0.0, delta));
            f.effect(no::EventId{f.run, applied.ordinal}, target, direct);
            links.push_back(f.core.last_ordinal());
            sum += delta;
        }
        target_context = f.evaluate(target, flat_book());
        const no::LiveRequest* live = f.core.find_live(target);
        REQUIRE(live != nullptr);
        REQUIRE(std::get<no::PendingDeferred>(live->pending).count == population);
        REQUIRE(std::holds_alternative<no::AllowanceDeferred>(live->allowance));
    }
};

// The ids the paged contract answers for one resolution event.
std::vector<std::uint64_t> visited_ids(const no::WorkingRequestCore& core,
                                       const no::EventId& resolution) {
    std::vector<std::uint64_t> ids;
    std::size_t foreign = 0;
    const bool known = core.visit_prior_adjustments(resolution, [&](const no::EventId& id) {
        if (!(id.run == core.identity())) ++foreign;
        ids.push_back(id.ordinal);
        return true;
    });
    CHECK(known);
    CHECK(foreign == 0);
    return ids;
}

// What a resolution event must answer, whatever its chain length.
void check_resolution(const no::WorkingRequestCore& core, const no::EventId& resolution,
                      const std::vector<std::uint64_t>& links,
                      const std::vector<no::EventId>& inline_ids) {
    const bool inline_chain = links.size() <= no::WorkingRequestCore::kInlineAdjustmentIds;
    CHECK(inline_ids.empty() == !inline_chain);        // a long chain is not copied into the event
    if (inline_chain) {
        REQUIRE(inline_ids.size() == links.size());
        for (std::size_t i = 0; i != links.size(); ++i) CHECK(inline_ids[i].ordinal == links[i]);
    }
    CHECK(core.prior_adjustment_count(resolution) == links.size());
    CHECK(visited_ids(core, resolution) == links);       // exact ids, exact chain order
    // The visitor stops when it says so.
    std::size_t seen = 0;
    const bool stopped = core.visit_prior_adjustments(resolution, [&](const no::EventId&) {
        return ++seen < 3;
    });
    CHECK(stopped);
    CHECK(seen == std::min<std::size_t>(3, links.size()));
}

// A resolution event is the only kind that answers.
void check_non_resolutions(const no::WorkingRequestCore& core) {
    const std::uint64_t first_ordinal = std::visit(
            [](const auto& payload) { return payload.ordinal; },
            core.history_at(core.history_base()));
    const no::EventId first{core.identity(), first_ordinal};
    const bool first_known = core.visit_prior_adjustments(
            first, [](const no::EventId&) { return true; });
    CHECK(!first_known);
    CHECK(core.prior_adjustment_count(first) == 0);
    const no::EventId unknown{core.identity(), std::uint64_t{1} << 40};
    const bool unknown_known = core.visit_prior_adjustments(
            unknown, [](const no::EventId&) { return true; });
    CHECK(!unknown_known);
    CHECK(core.prior_adjustment_count(unknown) == 0);
}

// Resolution by terms: a host-sized request takes its units.
void terms_resolution(std::size_t population, bool direct) {
    Chain c(population, direct);
    constexpr double kUnits = 1000000.0;
    const std::size_t before_end = c.f.core.history_end();
    const std::size_t keys_before = c.f.core.group_effect_receipt_count();
    {
        // Prepared and abandoned: nothing was written, no link was lost.
        auto abandoned = c.f.core.prepare_terms(c.target, c.target_context, terms(100.0, kUnits),
                                                c.f.ordinal);
        REQUIRE(std::holds_alternative<no::PreparedMutation>(abandoned));
    }
    CHECK(c.f.core.history_end() == before_end);
    CHECK(c.f.core.group_effect_receipt_count() == keys_before);
    {
        const no::LiveRequest* live = c.f.core.find_live(c.target);
        REQUIRE(live != nullptr);
        CHECK(std::get<no::PendingDeferred>(live->pending).count == population);
        CHECK(std::get<no::PendingDeferred>(live->pending).total == c.sum);
    }
    if (direct) {
        auto resolved = c.f.core.apply_terms(c.target, c.target_context, terms(100.0, kUnits),
                                             c.f.ordinal);
        REQUIRE(std::holds_alternative<no::Installed>(resolved));
        c.f.ordinal += std::get<no::Installed>(resolved).events.count;
    } else {
        c.f.install_mutation(c.f.core.prepare_terms(c.target, c.target_context,
                                                    terms(100.0, kUnits), c.f.ordinal));
    }
    const auto* receipt = std::get_if<no::TermsResolvedEvent>(&c.f.core.history_at(before_end));
    REQUIRE(receipt != nullptr);
    CHECK(receipt->pending_total == c.sum);
    CHECK(receipt->effective_deduction == c.sum);
    check_resolution(c.f.core, no::EventId{c.f.run, receipt->ordinal}, c.links,
                     receipt->prior_adjustment_ids);
    const no::LiveRequest* live = c.f.core.find_live(c.target);
    REQUIRE(live != nullptr);
    CHECK(std::holds_alternative<no::PendingNone>(live->pending));
    REQUIRE(std::holds_alternative<no::RemainingUnits>(live->remaining));
    CHECK(std::get<no::RemainingUnits>(live->remaining).q == kUnits - c.sum);
    check_non_resolutions(c.f.core);
}

// Resolution by the owner's fill: an owner-opened close takes the units its
// owner's fill opened, less the chain.
void owner_resolution(std::size_t population, bool direct) {
    Fixture f;
    no::Request parent_request{no::Transact{1000000.0}, "parent", ""};
    parent_request.trigger = no::Limit{50.0};
    const auto parent = f.submit(parent_request);
    REQUIRE(parent.handle.has_value());
    no::Request child_request{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
    child_request.owner = no::WaitForApplied{*parent.handle};
    child_request.group = no::Member{44, 2, no::GroupEffect::Reduce};
    const auto child = f.submit(child_request);
    REQUIRE(child.handle.has_value());
    std::vector<std::uint64_t> links;
    double sum = 0.0;
    for (std::size_t i = 0; i != population; ++i) {
        const double delta = (i % 3 == 0 ? 0.1 : i % 3 == 1 ? 0.2 : 0.3);
        no::Request source{no::Transact{delta}, "source", ""};
        source.group = no::Member{44, 1, no::GroupEffect::Reduce};
        const auto submitted = f.submit(source);
        REQUIRE(submitted.handle.has_value());
        const auto eval = f.evaluate(*submitted.handle, flat_book());
        const auto applied = f.apply(
                *submitted.handle,
                proposal_at(eval, no::ExecutionPlan{no::Transact{delta}}, 0.0, delta));
        f.effect(no::EventId{f.run, applied.ordinal}, *child.handle, direct);
        links.push_back(f.core.last_ordinal());
        sum += delta;
    }
    const auto parent_eval = f.evaluate(*parent.handle, flat_book());
    const auto parent_applied = f.apply(
            *parent.handle,
            proposal_at(parent_eval, no::ExecutionPlan{no::Transact{1000000.0}}, 0.0, 1000000.0));
    const no::EventId cause{f.run, parent_applied.ordinal};
    no::OpeningObservation opening;
    opening.queried_opening = *parent.handle;
    opening.queried_cycle = parent_applied.cycle_after;
    opening.current_position = no::PositionNonflat{parent_applied.cycle_after, no::Side::Long};
    opening.has_live_matching_lot = true;
    const std::size_t before_end = f.core.history_end();
    {
        auto abandoned = f.core.prepare_owner_applied(cause, *child.handle, opening, f.ordinal);
        REQUIRE(std::holds_alternative<no::PreparedMutation>(abandoned));
    }
    CHECK(f.core.history_end() == before_end);
    CHECK(std::get<no::PendingDeferred>(f.core.find_live(*child.handle)->pending).count
          == population);
    if (direct) {
        auto resolved = f.core.apply_owner_applied(cause, *child.handle, opening, f.ordinal);
        REQUIRE(std::holds_alternative<no::Installed>(resolved));
        f.ordinal += std::get<no::Installed>(resolved).events.count;
    } else {
        f.install_mutation(f.core.prepare_owner_applied(cause, *child.handle, opening, f.ordinal));
    }
    const auto* bound = std::get_if<no::QuantityBoundEvent>(&f.core.history_at(before_end));
    REQUIRE(bound != nullptr);
    CHECK(bound->pending_total == sum);
    CHECK(bound->effective_deduction == sum);
    CHECK(std::get<no::RemainingProjectionUnits>(bound->remaining).q == 1000000.0 - sum);
    check_resolution(f.core, no::EventId{f.run, bound->ordinal}, links, bound->prior_adjustment_ids);
    const no::LiveRequest* armed = f.core.find_live(*child.handle);
    REQUIRE(armed != nullptr);
    CHECK(std::holds_alternative<no::PendingNone>(armed->pending));
    CHECK(std::get<no::RemainingUnits>(armed->remaining).q == 1000000.0 - sum);
}

// An external list that cannot be written fails the prepare before anything is
// committed: the chain, the journal and the keys are as they were, and the
// same resolution succeeds once the scratch directory is back.
struct TempDirectory {
    std::string saved;
    bool had = false;
    explicit TempDirectory(const char* value) {
        const char* old = std::getenv("TMPDIR");
        had = old != nullptr;
        if (had) saved = old;
        ::setenv("TMPDIR", value, 1);
    }
    ~TempDirectory() {
        if (had) ::setenv("TMPDIR", saved.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
};

void unwritable_list_fails_before_commit(bool direct) {
    const std::size_t population = no::WorkingRequestCore::kInlineAdjustmentIds + 44;
    Chain c(population, direct);
    constexpr double kUnits = 1000000.0;
    const std::size_t before_end = c.f.core.history_end();
    const std::size_t keys_before = c.f.core.group_effect_receipt_count();
    const std::uint64_t ordinal_before = c.f.ordinal;
    bool refused = false;
    {
        const TempDirectory broken("/pineforge-o1e2-no-such-directory");
        try {
            if (direct) {
                (void) c.f.core.apply_terms(c.target, c.target_context, terms(100.0, kUnits),
                                            c.f.ordinal);
            } else {
                (void) c.f.core.prepare_terms(c.target, c.target_context, terms(100.0, kUnits),
                                              c.f.ordinal);
            }
        } catch (const std::system_error&) {
            refused = true;
        }
    }
    CHECK(refused);                                     // the scratch failure surfaced
    CHECK(c.f.core.history_end() == before_end);        // no partial journal row
    CHECK(c.f.core.group_effect_receipt_count() == keys_before);
    CHECK(c.f.ordinal == ordinal_before);
    const no::LiveRequest* live = c.f.core.find_live(c.target);
    REQUIRE(live != nullptr);
    CHECK(std::get<no::PendingDeferred>(live->pending).count == population);   // no lost link
    CHECK(std::get<no::PendingDeferred>(live->pending).total == c.sum);
    // The same command, once the directory is back.
    if (direct) {
        auto resolved = c.f.core.apply_terms(c.target, c.target_context, terms(100.0, kUnits),
                                             c.f.ordinal);
        REQUIRE(std::holds_alternative<no::Installed>(resolved));
    } else {
        c.f.install_mutation(c.f.core.prepare_terms(c.target, c.target_context,
                                                    terms(100.0, kUnits), c.f.ordinal));
    }
    const auto* receipt = std::get_if<no::TermsResolvedEvent>(&c.f.core.history_at(before_end));
    REQUIRE(receipt != nullptr);
    CHECK(visited_ids(c.f.core, no::EventId{c.f.run, receipt->ordinal}) == c.links);
    CHECK(receipt->pending_total == c.sum);
}
}  // namespace

int main() {
    try {
        for (const bool direct : {false, true}) {
            // Inside the inline bound, at it, and past it; and a chain long
            // enough that the journal it pins has spilled pages, so the ids
            // are read back across the external list and the external pages.
            for (const std::size_t population : {std::size_t{1}, std::size_t{2}, std::size_t{48},
                                                 no::WorkingRequestCore::kInlineAdjustmentIds,
                                                 no::WorkingRequestCore::kInlineAdjustmentIds + 1,
                                                 std::size_t{700}, std::size_t{6000}}) {
                terms_resolution(population, direct);
                owner_resolution(population, direct);
            }
            unwritable_list_fails_before_commit(direct);
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "o1-e2 chain-pages fixtures: %s\n", error.what());
    }
    std::printf("test_native_o1_chain_pages: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
