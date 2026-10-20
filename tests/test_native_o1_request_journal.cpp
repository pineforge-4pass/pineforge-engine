// CR-221 E02--E05. These fixtures deliberately use the base's public API:
// the RED is retained/temporary bytes after reaching the population, not a
// missing new declaration. Execution belongs to the supervisor's Linux proof.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <variant>
#include <vector>

namespace no = pineforge::native_order;
namespace {
int failures = 0;
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (false)
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error(#x); CHECK(x); } while (false)

struct Fixture {
    no::RunIdentity run{"o1-e2", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1, ordinal = 1;

    no::RequestHandle submit(no::Request request) {
        const auto result = core.submit(request, 0, incarnation, ordinal);
        REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle);
        return *result.handle;
    }
    void install(no::Preparation<no::PreparedMutation> preparation) {
        REQUIRE(std::holds_alternative<no::PreparedMutation>(preparation));
        const auto installed = core.install_mutation(
            std::get<no::PreparedMutation>(std::move(preparation)));
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ordinal += std::get<no::Installed>(installed).events.count;
    }
    no::EvaluationContext context() {
        no::EvaluationContext result;
        result.cursor.point.ordinal = ordinal++;
        result.cursor.point.effective_time_ms = 1000;
        result.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
        result.driver_class = no::DriverEligibilityClass::ObservedPrint;
        result.existing_matching_bit = true;
        return result;
    }
    no::EventId fill(no::RequestHandle handle, double units) {
        const auto evaluation = context();
        no::TargetObservation observation;
        observation.current_position = no::PositionNonflat{7, no::Side::Long};
        auto evaluated = core.prepare_evaluation(handle, evaluation, observation, ordinal);
        if (std::holds_alternative<no::PreparedMutation>(evaluated)) install(std::move(evaluated));
        else REQUIRE(std::holds_alternative<no::NoChange>(evaluated));
        no::ExecutionProposal proposal;
        proposal.cursor = evaluation.cursor;
        proposal.raw_price = proposal.resolved_price = 100;
        proposal.physical_action = no::Transact{units};
        proposal.pre_fill = observation.current_position;
        proposal.pre_target = observation;
        proposal.inspected_opened_units = units;
        auto prepared = core.prepare_execution(handle, proposal, ordinal);
        REQUIRE(std::holds_alternative<no::PreparedExecution>(prepared));
        no::CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = units;
        facts.cycle_before = facts.cycle_after = 7;
        facts.post_target = observation;
        facts.committed_action = no::Transact{units};
        auto installed = core.install_execution(
            std::get<no::PreparedExecution>(std::move(prepared)), facts);
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ordinal += std::get<no::Installed>(installed).events.count;
        return {run, core.last_ordinal()};
    }
};

std::size_t peak_bytes = 0;
void observe_peak(std::size_t) {
    peak_bytes = std::max(peak_bytes, global_allocation::live_bytes);
}

std::size_t live_book(std::size_t population) {
    global_allocation::recording = true;
    std::size_t retained = 0;
    {
        Fixture f;
        for (std::size_t i = 0; i != population; ++i) {
            no::Request request{no::Transact{1}, "book", "payload"};
            request.trigger = no::Limit{50};
            const auto handle = f.submit(request);
            CHECK(handle.incarnation == i + 1);
        }
        REQUIRE(f.core.live().size() == population); // E02 Q, before the bound.
        // A keep-handle replacement must move exactly this request to the end.
        const auto first = f.core.live().front().handle();
        no::Request replacement{no::Transact{2}, "repriced", "payload"};
        replacement.trigger = no::Limit{51};
        const auto result = f.core.replace(first, replacement, 0, f.incarnation,
                                           f.ordinal, no::ReplaceOptions{false, true, false});
        REQUIRE(result.status == no::ReplaceStatus::Replaced);
        CHECK(result.successor == first);
        CHECK(f.core.live().back().handle() == first);
        std::uint64_t expected = 2;
        for (const auto& row : f.core.live()) {
            if (row.handle() == first) CHECK(row.priority() == population + 1);
            else CHECK(row.handle().incarnation == expected++);
        }
        CHECK(expected == population + 1);
        (void)f.core.retire_history(f.core.last_ordinal());
        CHECK(f.core.history().empty());
        retained = global_allocation::live_bytes;
    }
    CHECK(global_allocation::live_bytes == 0);
    global_allocation::recording = false;
    return retained;
}

struct ChainResult { std::size_t retained, preparation_peak; };
ChainResult pinned_chain(std::size_t population, bool direct) {
    global_allocation::recording = true;
    ChainResult result{};
    {
        Fixture f;
        no::Request parent_request{no::Transact{10000}, "parent", ""};
        parent_request.trigger = no::Limit{50};
        const auto parent = f.submit(parent_request);
        no::Request child_request{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
        child_request.owner = no::WaitForApplied{parent};
        child_request.group = no::Member{7, 2, no::GroupEffect::Reduce};
        const auto child = f.submit(child_request);
        std::vector<no::EventId> expected_ids; // Oracle only; not core state.
        expected_ids.reserve(population);
        double sum = 0;
        no::EventId last_cause;
        for (std::size_t i = 0; i != population; ++i) {
            const double delta = (i % 3 == 0 ? 0.1 : i % 3 == 1 ? 0.2 : 0.3);
            no::Request source{no::Transact{delta}, "source", ""};
            source.group = no::Member{7, 1, no::GroupEffect::Reduce};
            last_cause = f.fill(f.submit(source), delta);
            if (direct) {
                const auto applied = f.core.apply_group_effect(last_cause, child, f.ordinal);
                REQUIRE(std::holds_alternative<no::Installed>(applied));
                f.ordinal += std::get<no::Installed>(applied).events.count;
            } else f.install(f.core.prepare_group_effect(last_cause, child, f.ordinal));
            expected_ids.push_back({f.run, f.core.last_ordinal()});
            sum += delta;
            (void)f.core.retire_history(f.core.last_ordinal());
        }
        const auto pending_live = f.core.find_live(child);
        REQUIRE(pending_live);
        const auto pending = std::get<no::PendingDeferred>(pending_live->pending);
        REQUIRE(pending.count == population); // E04 chain length.
        CHECK(pending.total == sum);
        REQUIRE(f.core.history().size() >= 3 * population - 2); // E03 pinned J.
        REQUIRE(f.core.group_effect_receipt_count() == population); // E05 keys.
        REQUIRE(f.core.group_effect_receipt_base() == 0);
        for (std::size_t i = 0; i != population; ++i) {
            const auto key = f.core.group_effect_receipt(i);
            CHECK(key.recipient == child && key.effect == no::GroupEffect::Reduce);
            CHECK(key.outcome_ordinal == expected_ids[i].ordinal);
        }
        const auto replay = f.core.prepare_group_effect(last_cause, child, f.ordinal);
        REQUIRE(std::holds_alternative<no::NoChange>(replay));
        CHECK(std::get<no::NoChange>(replay).reason == no::NoChangeReason::AlreadyApplied);
        // Exclude the test oracle from the retained-byte comparison.
        result.retained = global_allocation::live_bytes
            - expected_ids.capacity() * sizeof(no::EventId);
        const auto applied = f.fill(parent, 10000);
        const no::OpeningObservation opening{parent, 7, no::PositionNonflat{7, no::Side::Long}, true};
        const auto before = f.core.history_end();
        const auto receipt_count = f.core.group_effect_receipt_count();
        peak_bytes = global_allocation::live_bytes;
        const auto baseline = peak_bytes;
        global_allocation::on_recorded = observe_peak;
        {
            // Abandon a fully prepared chain resolution, then replay it.
            auto abandoned = f.core.prepare_owner_applied(applied, child, opening, f.ordinal);
            REQUIRE(std::holds_alternative<no::PreparedMutation>(abandoned));
            CHECK(f.core.history_end() == before);
            CHECK(f.core.group_effect_receipt_count() == receipt_count);
        }
        result.preparation_peak = peak_bytes - baseline;
        global_allocation::on_recorded = nullptr;
        CHECK(std::get<no::PendingDeferred>(f.core.find_live(child)->pending).count == population);
        f.install(f.core.prepare_owner_applied(applied, child, opening, f.ordinal));
        const auto bound = std::get<no::QuantityBoundEvent>(f.core.history_at(before));
        REQUIRE(bound.prior_adjustment_ids.size() == population);
        auto expected = expected_ids.begin();
        for (const auto& id : bound.prior_adjustment_ids) {
            REQUIRE(expected != expected_ids.end());
            CHECK(id == *expected++);
        }
        CHECK(expected == expected_ids.end());
        CHECK(bound.source_units == 10000 && bound.pending_total == sum);
        CHECK(bound.effective_deduction == sum);
        CHECK(std::get<no::RemainingProjectionUnits>(bound.remaining).q == 10000 - sum);
        CHECK(std::get<no::RemainingUnits>(f.core.find_live(child)->remaining).q == 10000 - sum);
    }
    CHECK(global_allocation::live_bytes == 0);
    global_allocation::recording = false;
    return result;
}
} // namespace

int main() {
    try {
        const auto small_book = live_book(64);
        const auto large_book = live_book(4096);
        CHECK(large_book <= small_book + 65536); // E02: independent of Q.
        for (bool direct : {false, true}) {
            const auto small = pinned_chain(64, direct);
            const auto large = pinned_chain(4096, direct);
            CHECK(large.retained <= small.retained + 65536); // E03 + E05, pinned J.
            CHECK(large.preparation_peak <= small.preparation_peak + 16384); // E04.
            std::printf("E03/E05 bytes %zu -> %zu; E04 prepare %zu -> %zu\n",
                        small.retained, large.retained,
                        small.preparation_peak, large.preparation_peak);
        }
    } catch (const std::exception& e) {
        ++failures;
        std::fprintf(stderr, "o1-e2 fixture: %s\n", e.what());
    }
    std::printf("test_native_o1_request_journal: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
