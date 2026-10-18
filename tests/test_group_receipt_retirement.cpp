// CR-150 L19B: group-effect keys leave RAM with their cause's journal window.
// The red checkpoint uses only the base API: a retired receipt is still
// readable there, and live heap grows with the number of completed drains.
// No compile/run result is claimed here; CORE-TOP executes both checkpoints.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PINEFORGE_LIBRARY_DEBUG_CHECKS
#error "tests/CMakeLists.txt must describe the library's Debug checks"
#endif

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                 \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
        ++failures;                                                           \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    }                                                                         \
} while (0)
#define REQUIRE(condition) do {                                               \
    const bool requirement = static_cast<bool>(condition);                     \
    CHECK(requirement);                                                       \
    if (!requirement) throw std::runtime_error(#condition);                    \
} while (0)

struct Fixture {
    no::RunIdentity run{"l19b", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t inc = 1;
    std::uint64_t ord = 1;
    std::uint64_t point = 1;
    std::uint64_t group = 0;
    bool nonflat = false;

    no::RequestHandle submit(no::Request request) {
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

    no::EventId fill(const no::RequestHandle& handle, double units) {
        const no::PositionIdentity before = nonflat
            ? no::PositionIdentity{no::PositionNonflat{1, no::Side::Long}}
            : no::PositionIdentity{no::PositionFlat{}};
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
        facts.cycle_before = nonflat ? 1 : 0;
        facts.cycle_after = 1;
        facts.post_target.current_position = no::PositionNonflat{1, no::Side::Long};
        facts.committed_action = no::Transact{units};
        const auto installed = core.install_execution(
            std::get<no::PreparedExecution>(std::move(prepared)), facts);
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ord += std::get<no::Installed>(installed).events.count;
        nonflat = true;
        const auto* event = std::get_if<no::ExecutionAppliedEvent>(&core.history().back());
        REQUIRE(event != nullptr);
        return no::EventId{run, event->ordinal};
    }

    bool already_applied(no::EventId cause, no::RequestHandle recipient, bool direct) {
        if (direct) {
            const auto result = core.apply_group_effect(cause, recipient, ord);
            const auto* unchanged = std::get_if<no::NoChange>(&result);
            return unchanged && unchanged->reason == no::NoChangeReason::AlreadyApplied;
        }
        const auto result = core.prepare_group_effect(cause, recipient, ord);
        const auto* unchanged = std::get_if<no::NoChange>(&result);
        return unchanged && unchanged->reason == no::NoChangeReason::AlreadyApplied;
    }

    void effect(no::EventId cause, no::RequestHandle recipient, bool direct) {
        if (direct) {
            const auto result = core.apply_group_effect(cause, recipient, ord);
            REQUIRE(std::holds_alternative<no::Installed>(result));
            ord += std::get<no::Installed>(result).events.count;
        } else {
            install(core.prepare_group_effect(cause, recipient, ord));
        }
    }

    struct Drain {
        no::EventId cause;
        std::vector<no::RequestHandle> recipients;
        std::size_t first = 0;
    };

    Drain drain(no::GroupEffect effect_kind, bool direct, bool apply = true) {
        Drain result;
        result.first = core.group_effect_receipt_count();
        ++group;
        for (std::int64_t cohort : {2, 3}) {
            no::Request request{no::Transact{10}, "sibling", ""};
            request.trigger = no::Limit{80};
            request.group = no::Member{group, cohort, effect_kind};
            result.recipients.push_back(submit(std::move(request)));
        }
        no::Request request{no::Transact{1}, "filler", ""};
        request.group = no::Member{group, 1, effect_kind};
        result.cause = fill(submit(std::move(request)), 1);
        REQUIRE(core.group_recipients(result.cause) == result.recipients);
        if (apply) {
            for (const auto& recipient : result.recipients) {
                effect(result.cause, recipient, direct);
            }
        }
        return result;
    }
};

bool same(const no::GroupEffectReceipt& a, const no::GroupEffectReceipt& b) {
    return a.cause == b.cause && a.recipient == b.recipient
        && a.effect == b.effect && a.outcome_ordinal == b.outcome_ordinal;
}

// Exercise the actual library, whose NDEBUG differs from this test's -UNDEBUG.
// Release must throw, Debug must abort at the below-base assertion. The base
// returns the retired key, so this child exits 2 in either profile (red).
void retired_index_is_rejected(const no::WorkingRequestCore& core, std::size_t index) {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        const rlimit no_core{0, 0};
        (void)::setrlimit(RLIMIT_CORE, &no_core);
        try {
            (void)core.group_effect_receipt(index);
        } catch (const std::out_of_range&) {
            ::_exit(0);
        } catch (...) {
            ::_exit(3);
        }
        ::_exit(2);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    REQUIRE(waited == child);
    if (PINEFORGE_LIBRARY_DEBUG_CHECKS) {
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    } else {
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

void receipt_window(no::GroupEffect effect, bool direct) {
    Fixture f;
    const auto first = f.drain(effect, direct);
    const auto second = f.drain(effect, direct);
    CHECK(f.core.group_effect_receipt_count() == 4);
    const auto retained = f.core.group_effect_receipt(second.first);
    // Retire the cause, even though its outcomes are still in the journal.
    REQUIRE(f.core.retire_history(first.cause.ordinal) > 0);
    CHECK(f.core.retired_through() == first.cause.ordinal);
    CHECK(f.core.group_effect_receipt_count() == 4);
    retired_index_is_rejected(f.core, first.first);
    CHECK(same(f.core.group_effect_receipt(second.first), retained));
    for (const auto& recipient : second.recipients) {
        CHECK(f.already_applied(second.cause, recipient, direct));
    }
    const auto invalid = f.core.prepare_group_effect(first.cause, first.recipients[0], f.ord);
    REQUIRE(std::holds_alternative<no::PreparationError>(invalid));
    CHECK(std::get<no::PreparationError>(invalid).code == no::CoreFailure::InvalidCause);
    CHECK(f.core.retire_history(first.cause.ordinal) == 0);
    bool past_end = false;
    try { (void)f.core.group_effect_receipt(4); }
    catch (const std::out_of_range&) { past_end = true; }
    CHECK(past_end);
    REQUIRE(f.core.retire_history(f.core.last_ordinal()) > 0);
    CHECK(f.core.group_effect_receipt_count() == 4);
    retired_index_is_rejected(f.core, second.first);
    const auto third = f.drain(effect, direct);
    CHECK(third.first == 4);
    CHECK(f.core.group_effect_receipt_count() == 6);
    CHECK(f.core.group_effect_receipt(4).cause == third.cause);
    CHECK(f.core.group_effect_receipt(5).recipient == third.recipients[1]);

    no::WorkingRequestCore moved(std::move(f.core));
    CHECK(moved.group_effect_receipt_count() == 6);
    CHECK(moved.group_effect_receipt(4).cause == third.cause);
    CHECK(moved.group_effect_receipt(4).recipient == third.recipients[0]);
    CHECK(f.core.group_effect_receipt_count() == 0);
    no::WorkingRequestCore assigned({"discarded", 1});
    assigned = std::move(moved);
    CHECK(assigned.group_effect_receipt_count() == 6);
    CHECK(assigned.group_effect_receipt(4).cause == third.cause);
    CHECK(moved.group_effect_receipt_count() == 0);
    assigned.reset({"l19b", 2});
    CHECK(assigned.group_effect_receipt_count() == 0);
}

void a_prepared_effect_cannot_restore_a_retired_cause() {
    Fixture f;
    const auto drain = f.drain(no::GroupEffect::Cancel, false, false);
    auto prepared = f.core.prepare_group_effect(drain.cause, drain.recipients[0], f.ord);
    REQUIRE(std::holds_alternative<no::PreparedMutation>(prepared));
    REQUIRE(f.core.retire_history(f.core.last_ordinal()) > 0);
    const auto result = f.core.install_mutation(
        std::get<no::PreparedMutation>(std::move(prepared)));
    REQUIRE(std::holds_alternative<no::InstallError>(result));
    CHECK(std::get<no::InstallError>(result) == no::InstallError::StalePreparation);
    CHECK(f.core.group_effect_receipt_count() == 0);
    CHECK(f.core.find_live(drain.recipients[0]) != nullptr);
}

std::vector<double> deferred_chain(bool retire, bool direct) {
    Fixture f;
    no::Request parent_request{no::Transact{4}, "parent", ""};
    parent_request.trigger = no::Limit{50};
    const auto parent = f.submit(std::move(parent_request));
    std::vector<no::RequestHandle> children;
    for (std::int64_t cohort : {2, 3}) {
        no::Request request{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
        request.owner = no::WaitForApplied{parent};
        request.group = no::Member{1, cohort, no::GroupEffect::Reduce};
        children.push_back(f.submit(std::move(request)));
    }
    no::Request filler{no::Transact{1}, "filler", ""};
    filler.group = no::Member{1, 1, no::GroupEffect::Reduce};
    const auto cause = f.fill(f.submit(std::move(filler)), 1);
    for (const auto& child : children) f.effect(cause, child, direct);
    std::uint64_t head = 0;
    for (const auto& event : f.core.history()) {
        if (const auto* pending = std::get_if<no::DeferredGroupAdjustmentEvent>(&event)) {
            head = pending->ordinal;
            break;
        }
    }
    REQUIRE(head != 0);
    if (retire) {
        REQUIRE(f.core.retire_history(std::numeric_limits<std::uint64_t>::max()) > 0);
        CHECK(f.core.retired_through() == head - 1);
        CHECK(f.core.event_at(no::EventId{f.run, head}) != nullptr);
        CHECK(f.core.group_effect_receipt_count() == 2);
        retired_index_is_rejected(f.core, 0);
    }
    const auto applied = f.fill(parent, 4);
    std::vector<double> answer;
    for (const auto& child : children) {
        no::OpeningObservation opening;
        opening.queried_opening = parent;
        opening.queried_cycle = 1;
        opening.current_position = no::PositionNonflat{1, no::Side::Long};
        opening.has_live_matching_lot = true;
        const auto start = f.core.history_end();
        f.install(f.core.prepare_owner_applied(applied, child, opening, f.ord));
        for (auto index = start; index < f.core.history_end(); ++index) {
            if (const auto* bound = std::get_if<no::QuantityBoundEvent>(&f.core.history_at(index))) {
                CHECK(bound->prior_adjustment_ids.size() == 1);
                answer.push_back(bound->pending_total);
                answer.push_back(bound->effective_deduction);
            }
        }
        const auto* live = f.core.find_live(child);
        REQUIRE(live != nullptr);
        REQUIRE(std::holds_alternative<no::RemainingUnits>(live->remaining));
        answer.push_back(std::get<no::RemainingUnits>(live->remaining).q);
    }
    REQUIRE(answer.size() == 6);
    return answer;
}

std::size_t live_heap_after_drains(int drains) {
    REQUIRE(global_allocation::live_bytes == 0);
    global_allocation::recording = true;
    std::size_t bytes = 0;
    {
        Fixture f;
        for (int index = 0; index < drains; ++index) {
            (void)f.drain(no::GroupEffect::Cancel, index % 2 == 0);
            REQUIRE(f.core.retire_history(f.core.last_ordinal()) > 0);
        }
        CHECK(f.core.group_effect_receipt_count() == static_cast<std::size_t>(drains) * 2);
        CHECK(f.core.history().empty());
        CHECK(f.core.live().empty());
        bytes = global_allocation::live_bytes;
    }
    CHECK(global_allocation::live_bytes == 0);
    global_allocation::recording = false;
    return bytes;
}

struct ConsumerRun {
    k3_book::Outcome outcome;
    std::size_t receipts = 0;
};

ConsumerRun consumer_run(const k3_book::BookConfig& config, bool window,
                         bool direct, bool every_point) {
    k3_book::BookHost host(config);
    auto& consumer = as_native_consumer(host.execution_consumer_for_test());
    consumer.set_retention_override(window ? NativeEventRetention::Window
                                          : NativeEventRetention::Commands);
    consumer.set_retire_every_point(every_point);
    consumer.set_direct_mutation(direct);
    const auto tape = k3_book::make_tape(config);
    const auto spec = k3_book::make_spec(config, tape);
    REQUIRE(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    host.finish();
    REQUIRE(host.outcome.completed);
    REQUIRE(host.outcome.error.empty());
    return {host.outcome, consumer.request_core().group_effect_receipt_count()};
}

void consumer_digests_survive_retirement() {
    for (const auto path : {k3_book::Path::None, k3_book::Path::Synthesized,
                            k3_book::Path::Lower}) {
        k3_book::BookConfig config;
        config.seed = 303;
        config.live = 20;
        config.bars = 60;
        config.calc_on_fills = true;
        config.path = path;
        const auto retained = consumer_run(config, false, false, false);
        REQUIRE(retained.receipts > 0);
        for (bool direct : {false, true}) {
            for (bool stress : {false, true}) {
                const auto window = consumer_run(config, true, direct, stress);
                CHECK(window.receipts == retained.receipts);
                CHECK(window.outcome.trace == retained.outcome.trace);
                CHECK(window.outcome.continuation == retained.outcome.continuation);
                CHECK(window.outcome.broker == retained.outcome.broker);
                CHECK(window.outcome.trades == retained.outcome.trades);
                CHECK(window.outcome.trades_digest == retained.outcome.trades_digest);
                CHECK(window.outcome.position == retained.outcome.position);
            }
        }
    }
}
}  // namespace

int main() {
    try {
        for (bool direct : {false, true}) {
            for (auto effect : {no::GroupEffect::Cancel, no::GroupEffect::Reduce}) {
                receipt_window(effect, direct);
            }
            CHECK(deferred_chain(true, direct) == deferred_chain(false, direct));
        }
        const auto small = live_heap_after_drains(64);
        const auto large = live_heap_after_drains(4096);
        CHECK(large <= small + 4096);
        std::printf("L19B receipt heap: 64 -> 4096 drains, %zu -> %zu live bytes\n", small, large);
        consumer_digests_survive_retirement();
        a_prepared_effect_cannot_restore_a_retired_cause();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "L19B fixture stopped: %s\n", error.what());
    }
    std::printf("test_group_receipt_retirement: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
