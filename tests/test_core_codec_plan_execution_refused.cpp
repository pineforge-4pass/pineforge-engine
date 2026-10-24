// 1.7.0 review B1-03, the commit half, option (d) (L6 decision; a behaviour
// change named for the release notes): with a journal codec bound, the
// plan-form execution is refused at its prepare, typed, with the core as it
// was.
//
// prepare_execution + install_execution is the plan form of an execution. Its
// ExecutionAppliedEvent is final only inside install_execution, after the
// settlement, and that install is noexcept: with a journal codec bound
// (bind_journal_codec) the codec's store write would run there with no phase
// that can fail, so a store failure ended in std::terminate. In 1.7.0 the
// plan form's prepare refuses a codec-bound core with
// PreparationError{CoreFailure::UnsupportedTransition} before anything moves;
// the direct form (check_execution + apply_execution) writes its one event
// before it publishes and serves such a core. No shipped host binds a codec.
//
// The run (the core alone, its public C++ API):
//  - refused: a codec-bound core, a market request evaluated to its point
//    and a resting one; a cancel of the resting one is prepared first, as a
//    probe of the core's epoch. Both prepare_execution overloads must answer
//    the typed refusal, the journal, the store, the counters and the row must
//    stay as they were, and the probe must still install (nothing moved).
//  - control, direct form: the same request on a codec-bound core executes
//    through check_execution + apply_execution.
//  - control, no codec: the plan form is unchanged on a core with no codec.
//
// Previous-epoch API only, so this TU compiles against the unfixed head and
// fails there on the refusal.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/native_order.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace pineforge::detail;
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

constexpr std::size_t kPage = 512;

// The journal codec's keys are 8-byte little-endian positions.
ScratchStatus numeric_order(void*, const ScratchByteSource& a, const ScratchByteSource& b,
                            int& order) noexcept {
    if (a.size() != 8 || b.size() != 8) return ScratchStatus::programming_error;
    std::array<std::byte, 8> left{}, right{};
    auto status = a.read(0, {left.data(), left.size()});
    if (status != ScratchStatus::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != ScratchStatus::ok) return status;
    order = 0;
    for (std::size_t i = 8; i-- != 0;) {
        if (left[i] != right[i]) {
            order = std::to_integer<unsigned>(left[i]) < std::to_integer<unsigned>(right[i]) ? -1 : 1;
            break;
        }
    }
    return ScratchStatus::ok;
}

no::Request market_buy() { return no::Request{no::Transact{1.0}, "buy", ""}; }

no::Request resting_buy() {
    no::Request request{no::Transact{1.0}, "rest", ""};
    request.trigger = no::Limit{50.0};
    return request;
}

struct Driver {
    no::WorkingRequestCore& core;
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;

    no::RequestHandle submit(const no::Request& request) {
        const auto result = core.submit(request, 0, incarnation, ordinal);
        REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle.has_value());
        return *result.handle;
    }
    // Evaluates `handle` at a fresh point from a flat book and answers the
    // proposal of one unit at 100 there.
    no::ExecutionProposal evaluate(const no::RequestHandle& handle) {
        no::EvaluationContext context;
        context.cursor.point.ordinal = ordinal + 1;
        context.cursor.point.effective_time_ms = 1000;
        context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
        context.driver_class = no::DriverEligibilityClass::ObservedPrint;
        context.existing_matching_bit = true;
        no::TargetObservation target;
        target.current_position = no::PositionFlat{};
        auto evaluation = core.prepare_evaluation(handle, context, target, ordinal);
        if (std::holds_alternative<no::PreparedMutation>(evaluation)) {
            auto installed = core.install_mutation(
                std::get<no::PreparedMutation>(std::move(evaluation)));
            REQUIRE(std::holds_alternative<no::Installed>(installed));
            ordinal += std::get<no::Installed>(installed).events.count;
        } else {
            REQUIRE(std::holds_alternative<no::NoChange>(evaluation));
        }
        no::ExecutionProposal proposal;
        proposal.cursor = context.cursor;
        proposal.raw_price = 100;
        proposal.resolved_price = 100;
        proposal.physical_action = no::Transact{1.0};
        proposal.pre_fill = target.current_position;
        proposal.pre_target = target;
        proposal.inspected_opened_units = 1.0;
        proposal.inspected_current_ticket = 1;
        return proposal;
    }
    static no::CommittedExecutionFacts facts() {
        no::CommittedExecutionFacts out;
        out.result.status = pineforge::execution::Status::Applied;
        out.result.opened_units = 1.0;
        out.result.current_ticket = 1;
        out.cycle_before = 0;
        out.cycle_after = 1;
        out.post_target.current_position = no::PositionNonflat{1, no::Side::Long};
        out.committed_action = no::Transact{1.0};
        return out;
    }
};

void check_refusal(const no::Preparation<no::PreparedExecution>& answer,
                   const no::RequestHandle& target, const char* form) {
    const auto* error = std::get_if<no::PreparationError>(&answer);
    if (!error) std::fprintf(stderr, "%s: the plan form was not refused\n", form);
    CHECK(error != nullptr);
    if (!error) return;
    CHECK(error->code == no::CoreFailure::UnsupportedTransition);
    CHECK(error->target == target);
}

void plan_execution_refused_with_codec() {
    std::vector<std::byte> storage(1024 * kPage);
    FixedMemoryScratchBacking memory({storage.data(), storage.size()}, kPage);
    CommandScratchStore store{
        {2, kPage, 1, 1, {0x62312d3033726574ULL, nullptr, &numeric_order},
         ScratchOpenMode::create_empty},
        &memory};
    REQUIRE(store.status() == ScratchStatus::ok);

    no::RunIdentity run{"b1-03d-plan-refused", 1};
    no::WorkingRequestCore core(run);
    no::bind_journal_codec(core, store);
    Driver d{core};
    const auto target = d.submit(market_buy());
    const auto other = d.submit(resting_buy());
    const auto proposal = d.evaluate(target);

    // A token prepared before the refusal installs only if nothing moved the
    // core since (a promotion or a seal raises the epoch it is checked by).
    auto probe = core.prepare_cancel(other, d.ordinal);
    const std::size_t end_before = core.history_end();
    const std::size_t base_before = core.history_base();
    const std::uint64_t last_before = core.last_ordinal();
    const std::size_t live_before = core.live().size();
    const std::uint64_t records_before = store.record_count();
    const no::LiveRequest* row = core.find_live(target);
    REQUIRE(row != nullptr);
    const no::LiveRequest row_before = *row;

    check_refusal(core.prepare_execution(target, proposal, d.ordinal), target, "plain");
    check_refusal(core.prepare_execution(target, proposal, d.ordinal, true), target, "granted");

    // The core is as it was.
    CHECK(core.history_end() == end_before);
    CHECK(core.history_base() == base_before);
    CHECK(core.last_ordinal() == last_before);
    CHECK(core.live().size() == live_before);
    CHECK(store.record_count() == records_before);
    row = core.find_live(target);
    CHECK(row != nullptr);
    if (row) {
        CHECK(row->remaining.index() == row_before.remaining.index());
        CHECK(row->allowance.index() == row_before.allowance.index());
        CHECK(row->trigger_state.index() == row_before.trigger_state.index());
        const auto* units = std::get_if<no::RemainingUnits>(&row->remaining);
        const auto* units_before = std::get_if<no::RemainingUnits>(&row_before.remaining);
        CHECK((units == nullptr) == (units_before == nullptr));
        if (units && units_before) CHECK(units->q == units_before->q);
        const auto* allowance = std::get_if<no::AllowanceUnits>(&row->allowance);
        const auto* allowance_before = std::get_if<no::AllowanceUnits>(&row_before.allowance);
        CHECK((allowance == nullptr) == (allowance_before == nullptr));
        if (allowance && allowance_before) {
            CHECK(allowance->point_ordinal == allowance_before->point_ordinal);
            CHECK(allowance->left == allowance_before->left);
        }
    }
    const auto installed = core.install_cancel(std::move(probe));
    CHECK(!std::holds_alternative<no::InstallError>(installed));
}

void direct_execution_serves_a_codec_core() {
    std::vector<std::byte> storage(1024 * kPage);
    FixedMemoryScratchBacking memory({storage.data(), storage.size()}, kPage);
    CommandScratchStore store{
        {2, kPage, 1, 1, {0x62312d3033726574ULL, nullptr, &numeric_order},
         ScratchOpenMode::create_empty},
        &memory};
    REQUIRE(store.status() == ScratchStatus::ok);

    no::RunIdentity run{"b1-03d-direct", 1};
    no::WorkingRequestCore core(run);
    no::bind_journal_codec(core, store);
    Driver d{core};
    const auto target = d.submit(market_buy());
    const auto proposal = d.evaluate(target);
    const auto checked = core.check_execution(target, proposal, d.ordinal);
    REQUIRE(std::holds_alternative<std::monostate>(checked));
    const auto installed = core.apply_execution(target, proposal, Driver::facts(), d.ordinal);
    REQUIRE(std::holds_alternative<no::Installed>(installed));
    const auto range = std::get<no::Installed>(installed).events;
    const auto* applied =
        std::get_if<no::ExecutionAppliedEvent>(&core.history_at(range.first_index));
    CHECK(applied != nullptr);
    if (applied) {
        CHECK(applied->handle() == target);
        CHECK(applied->filled_working == 1.0);
        CHECK(applied->terminal);
    }
}

void plan_execution_unchanged_without_codec() {
    no::RunIdentity run{"b1-03d-no-codec", 1};
    no::WorkingRequestCore core(run);
    Driver d{core};
    const auto target = d.submit(market_buy());
    const auto proposal = d.evaluate(target);
    auto prepared = core.prepare_execution(target, proposal, d.ordinal);
    REQUIRE(std::holds_alternative<no::PreparedExecution>(prepared));
    const auto installed = core.install_execution(
        std::get<no::PreparedExecution>(std::move(prepared)), Driver::facts());
    REQUIRE(std::holds_alternative<no::Installed>(installed));
    const auto range = std::get<no::Installed>(installed).events;
    const auto* applied =
        std::get_if<no::ExecutionAppliedEvent>(&core.history_at(range.first_index));
    CHECK(applied != nullptr && applied->handle() == target);
}

}  // namespace

int main() {
    const struct {
        const char* name;
        void (*run)();
    } rows[] = {
        {"plan execution refused with a codec", plan_execution_refused_with_codec},
        {"direct execution serves a codec core", direct_execution_serves_a_codec_core},
        {"plan execution unchanged without a codec", plan_execution_unchanged_without_codec},
    };
    for (const auto& row : rows) {
        try {
            row.run();
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "%s: %s\n", row.name, error.what());
        }
    }
    std::printf("test_core_codec_plan_execution_refused: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
