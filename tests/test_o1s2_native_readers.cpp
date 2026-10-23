// LB-446: source only. On the frozen base this TU must fail to compile at
// OpeningObservationReadback / OpeningInputCursor and the visitor members.
#include "native_current_fixture.hpp"
#include "global_allocation_replacement.hpp"

#include <array>
#include <cstring>

using namespace r4_test;
namespace {
using Status = pineforge::detail::ScratchStatus;

struct NoAlloc {
    std::size_t before = global_allocation::allocations;
    ~NoAlloc() { CHECK(before == global_allocation::allocations); }
};
bool bits(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

struct Inputs : no::OpeningObservationReadback {
    no::RunIdentity run{"o1s2-openings", 1};
    std::uint64_t size = 17;
    bool alive = true;
    bool poisoned = false;
    bool duplicate = false;
    std::uint64_t count() const noexcept override { return size; }
    Status observation_at(std::uint64_t i, no::OpeningObservation& out) const noexcept override {
        if (i >= size) return Status::not_found;
        out = {{run, i + 1}, 7, no::PositionNonflat{7, no::Side::Long}, alive};
        return Status::ok;
    }
    static Status read(void* context, std::uint64_t i, no::RequestHandle& out) {
        auto& input = *static_cast<Inputs*>(context);
        if (input.poisoned) return Status::io_error;
        if (i >= input.size) return Status::not_found;
        out = {input.run, input.duplicate ? 1 : input.size - i};
        return Status::ok;
    }
};

void openings_and_observations() {
    for (auto n : {17U, 129U, 513U}) {
        Inputs input;
        input.size = n;
        no::WorkingRequestCore core(input.run);
        std::uint64_t incarnation = 1000, ordinal = 1;
        auto cursor = std::make_shared<no::OpeningInputCursor>(n, &input, Inputs::read);
        no::Request request{no::Reduce{no::ExplicitUnits{1}}, "close", "frozen"};
        request.owner = no::BindOpenings{{}, 7, cursor};
        no::CommandContext command;
        command.external_openings = &input;
        auto installed = core.apply_submit(request, command, incarnation, ordinal);
        REQUIRE(installed.result.status == no::SubmitStatus::Accepted);
        REQUIRE(installed.result.handle);
        const auto* live = core.find_live(*installed.result.handle);
        REQUIRE(live);
        const auto& owner = std::get<no::BindOpenings>(live->request().owner);
        const auto& authority = std::get<no::OpeningsClose>(live->authority);
        REQUIRE(owner.openings.empty() && authority.openings.empty());
        REQUIRE(owner.external_openings && authority.external_openings);
        CHECK(owner.external_openings.get() != cursor.get());
        CHECK(owner.external_openings == authority.external_openings);
        input.poisoned = true;
        {
            NoAlloc guard;
            no::RequestHandle unchanged{input.run, 991};
            CHECK(cursor->opening_at(0, unchanged) == Status::io_error);
            CHECK(unchanged.incarnation == 991);
        }
        request.owner = no::Independent{};
        cursor.reset();
        no::TargetObservation observed;
        observed.current_position = no::PositionNonflat{7, no::Side::Long};
        observed.external_openings = &input;
        {
            NoAlloc guard;
            CHECK(no::opening_observation_count(observed) == n);
            for (std::uint64_t i = 0; i != n; ++i) {
                no::RequestHandle got;
                CHECK(no::opening_at(owner.openings, owner.external_openings.get(), i, got) == Status::ok);
                CHECK(got == (no::RequestHandle{input.run, i + 1}));
                no::OpeningObservation observation;
                CHECK(no::opening_observation_at(observed, i, observation) == Status::ok);
                CHECK(observation.queried_opening == got && observation.has_live_matching_lot);
            }
            no::RequestHandle unchanged{input.run, 999};
            CHECK(authority.external_openings->opening_at(n, unchanged) == Status::not_found);
            CHECK(unchanged.incarnation == 999);
        }
        // The real evaluator must use the borrowed observations, including a
        // later completely retired scope, rather than the empty vector.
        input.alive = false;
        no::EvaluationContext evaluation;
        evaluation.cursor.point.ordinal = 5000;
        evaluation.existing_matching_bit = true;
        ordinal += installed.events.count;
        auto result = core.apply_evaluation(*installed.result.handle, evaluation, observed, ordinal);
        CHECK(std::holds_alternative<no::Installed>(result));
        CHECK(!core.find_live(*installed.result.handle));

        std::shared_ptr<const no::OpeningReadback> frozen = owner.external_openings;
        const auto previous = frozen;
        no::OpeningInputCursor broken(n, &input, Inputs::read);
        CHECK(no::freeze_openings(input.run, broken, frozen) == Status::io_error);
        CHECK(frozen == previous);
        input.poisoned = false;
        input.duplicate = true;
        CHECK(no::freeze_openings(input.run, broken, frozen) == Status::programming_error);
        CHECK(frozen == previous);
    }
}

no::SubmitResult submit_core(no::WorkingRequestCore& core, const no::Request& request,
        const no::CommandContext& context, std::uint64_t& incarnation, std::uint64_t& ordinal) {
    auto result = core.apply_submit(request, context, incarnation, ordinal);
    ordinal += result.events.count;
    if (result.result.handle) ++incarnation;
    return result.result;
}

void core_handle_visitors() {
    const no::RunIdentity run{"o1s2-core-visitors", 1};
    no::WorkingRequestCore core(run);
    std::uint64_t incarnation = 1, ordinal = 1;
    const auto parent = submit_core(core, tx(1), {}, incarnation, ordinal);
    REQUIRE(parent.handle);
    for (unsigned i = 0; i != 33; ++i) {
        auto child = reduce(1);
        child.owner = no::WaitForApplied{*parent.handle};
        REQUIRE(submit_core(core, child, {}, incarnation, ordinal).handle);
    }
    no::CommandContext observed;
    observed.opening = no::OpeningObservation{{run, 999}, 7, no::PositionNonflat{7, no::Side::Long}, true};
    auto close = reduce(1);
    close.owner = no::BindOpening{{run, 999}, 7};
    REQUIRE(submit_core(core, close, observed, incarnation, ordinal).handle);
    auto repriced_child = reduce(1);
    repriced_child.owner = no::WaitForApplied{*parent.handle};
    auto repriced = core.apply_replace({run, 2}, repriced_child, {}, incarnation, ordinal,
                                      no::ReplaceOptions{false, true});
    REQUIRE(repriced.result.status == no::ReplaceStatus::Replaced);
    ordinal += repriced.events.count;
    ++incarnation;
    const auto expected_children = core.waiting_children(*parent.handle);
    const auto expected_bound = core.bound_close_handles();
    REQUIRE(expected_children.size() == 33 && expected_bound.size() == 1);
    for (unsigned i = 0; i != 32; ++i) CHECK(expected_children[i].incarnation == i + 3);
    CHECK(expected_children.back().incarnation == 2);
    struct Compare {
        const std::vector<no::RequestHandle>& expected;
        std::size_t index = 0;
        static void read(void* context, const no::RequestHandle& handle) {
            auto& c = *static_cast<Compare*>(context);
            REQUIRE(c.index < c.expected.size());
            CHECK(handle == c.expected[c.index++]);
        }
    };
    {
        NoAlloc guard;
        Compare children{expected_children}, bound{expected_bound};
        core.visit_waiting_children(*parent.handle, &children, Compare::read);
        core.visit_bound_close_handles(&bound, Compare::read);
        CHECK(children.index == 33 && bound.index == 1);
    }

    // Produce a real applied cause. Its own group has two other cohorts, in
    // queue order, while an unrelated group and same-cohort member are absent.
    no::WorkingRequestCore grouped(run);
    incarnation = ordinal = 1;
    auto cause_request = tx(2);
    cause_request.capacity = no::PointBudget{1};
    cause_request.group = no::Member{41, 1, no::GroupEffect::Reduce};
    const auto cause = submit_core(grouped, cause_request, {}, incarnation, ordinal);
    REQUIRE(cause.handle);
    for (unsigned i = 0; i != 4; ++i) {
        auto sibling = tx(5);
        sibling.group = no::Member{i == 3 ? 42U : 41U, i == 2 ? 1 : int(i + 2), no::GroupEffect::Reduce};
        REQUIRE(submit_core(grouped, sibling, {}, incarnation, ordinal).handle);
    }
    no::EvaluationContext evaluation;
    evaluation.cursor.point.ordinal = 100;
    evaluation.existing_matching_bit = true;
    grouped.apply_evaluation(*cause.handle, evaluation, {}, ordinal);
    ordinal += 1000;
    no::ExecutionProposal proposal;
    proposal.cursor = evaluation.cursor;
    proposal.raw_price = proposal.resolved_price = 100;
    proposal.physical_action = no::Transact{1};
    proposal.inspected_opened_units = 1;
    proposal.inspected_current_ticket = 1;
    auto preparation = grouped.prepare_execution(*cause.handle, proposal, ordinal);
    REQUIRE(std::holds_alternative<no::PreparedExecution>(preparation));
    no::CommittedExecutionFacts facts;
    facts.result.status = ex::Status::Applied;
    facts.result.opened_units = 1;
    facts.result.current_ticket = 1;
    facts.cycle_after = 1;
    facts.post_target.current_position = no::PositionNonflat{1, no::Side::Long};
    facts.committed_action = no::Transact{1};
    auto installed = grouped.install_execution(std::get<no::PreparedExecution>(std::move(preparation)), facts);
    REQUIRE(std::holds_alternative<no::Installed>(installed));
    const auto& applied = std::get<no::ExecutionAppliedEvent>(grouped.history().back());
    const no::EventId event{run, applied.ordinal};
    const auto expected_group = grouped.group_recipients(event);
    REQUIRE(expected_group.size() == 2);
    CHECK(expected_group[0].incarnation == 2 && expected_group[1].incarnation == 3);
    {
        NoAlloc guard;
        Compare group{expected_group};
        grouped.visit_group_recipients(event, &group, Compare::read);
        CHECK(group.index == 2);
    }
}

void ordinary_run_visitors() {
    for (auto n : {17U, 129U, 513U}) {
        Host host;
        std::string text(4097, 'x');
        text[129] = '\0';
        no::RequestHandle hidden;
        unsigned observations = 0;
        host.calculation = [&](Host& h) {
            if (h.calculations == 1) {
                no::RequestHandle pending_parent;
                for (unsigned i = 0; i != n; ++i) {
                    auto opening = tx(1);
                    opening.label = text;
                    opening.comment = text;
                    put(h, opening);
                    opening.trigger = no::Limit{1};
                    pending_parent = put(h, opening);
                }
                auto child = reduce(1);
                child.owner = no::WaitForApplied{pending_parent, no::NativeArmVisibility::PendingUntilArmed};
                hidden = put(h, child);
                return;
            }
            ++observations;
            const auto working = h.native_working_requests();
            REQUIRE(working.size() == n);
            REQUIRE(h.physical_position().lot_count == n);
            for (unsigned i = 0; i != n; ++i)
                CHECK(working[i].definition->handle.incarnation == 2 * i + 2);
            struct Work {
                const Host& host;
                const std::vector<NativeWorkingRequest>& expected;
                std::size_t index = 0;
                static bool read(void* context, const NativeWorkingRequest& row) {
                    auto& c = *static_cast<Work*>(context);
                    REQUIRE(c.index < c.expected.size());
                    CHECK(row.definition == c.expected[c.index].definition);
                    CHECK(row.remaining.index() == c.expected[c.index].remaining.index());
                    if (const auto* units = std::get_if<no::RemainingProjectionUnits>(&row.remaining)) {
                        REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(
                            c.expected[c.index].remaining));
                        CHECK(bits(units->q, std::get<no::RemainingProjectionUnits>(
                            c.expected[c.index].remaining).q));
                    }
                    const auto point = c.host.native_working_request(row.definition->handle);
                    CHECK(point && point->definition == row.definition);
                    ++c.index;
                    return true;
                }
                static void read_void(void* context, const NativeWorkingRequest& row) { read(context, row); }
            };
            {
                NoAlloc guard;
                Work call{h, working};
                CHECK(h.visit_native_working_requests(&call, Work::read) == Status::ok);
                CHECK(call.index == n);
                call.index = 0;
                h.native_visit_working_requests(&call, Work::read_void);
                CHECK(call.index == n);
                CHECK(!h.native_working_request(hidden));
                auto foreign = working.front().definition->handle;
                ++foreign.run.run_number;
                CHECK(!h.native_working_request(foreign));
                std::size_t count = 0;
                CHECK(h.visit_native_working_requests(&count, [](void* context, const NativeWorkingRequest&) {
                    ++*static_cast<std::size_t*>(context); return false;
                }) == Status::cancelled);
                CHECK(count == 1);
            }
            for (double mark : {101.0, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity()}) {
                const auto expected = h.native_open_lots(mark);
                for (unsigned i = 0; i != n; ++i) CHECK(expected[i].entry_incarnation == 2 * i + 1);
                struct Lots {
                    const std::vector<NativeOpenLot>& expected;
                    std::size_t index = 0;
                    static bool read(void* context, const NativeOpenLotView& row) {
                        auto& c = *static_cast<Lots*>(context);
                        REQUIRE(c.index < c.expected.size());
                        const auto& old = c.expected[c.index++];
                        CHECK(row.ordinal == old.ordinal && row.entry_incarnation == old.entry_incarnation);
                        CHECK(row.cycle == old.cycle && row.side == old.side);
                        CHECK(row.entry_label == std::string_view(old.entry_label));
                        CHECK(row.entry_comment == std::string_view(old.entry_comment));
                        CHECK(row.entry_time_ms == old.entry_time_ms && row.entry_bar_index == old.entry_bar_index);
                        CHECK(bits(row.entry_price, old.entry_price) && bits(row.signed_units, old.signed_units));
                        CHECK(bits(row.entry_commission, old.entry_commission) && bits(row.mark, old.mark));
                        CHECK(bits(row.unrealized_pnl, old.unrealized_pnl));
                        CHECK(bits(row.favorable_excursion, old.favorable_excursion));
                        CHECK(bits(row.adverse_excursion, old.adverse_excursion));
                        return true;
                    }
                    static void read_void(void* context, const NativeOpenLotView& row) { read(context, row); }
                    static bool values(void* context, std::uint64_t incarnation, double price, double units) {
                        auto& c = *static_cast<Lots*>(context);
                        REQUIRE(c.index < c.expected.size());
                        const auto& old = c.expected[c.index++];
                        CHECK(incarnation == old.entry_incarnation);
                        CHECK(bits(price, old.entry_price) && bits(units, old.signed_units));
                        return true;
                    }
                };
                NoAlloc guard;
                Lots call{expected};
                CHECK(h.visit_native_open_lots(mark, &call, Lots::read) == Status::ok);
                CHECK(call.index == n);
                call.index = 0;
                h.native_visit_open_lots(mark, &call, Lots::read_void);
                CHECK(call.index == n);
                call.index = 0;
                CHECK(h.visit_native_open_lot_values(&call, Lots::values) == Status::ok);
                CHECK(call.index == n);
                std::size_t count = 0;
                CHECK(h.visit_native_open_lots(mark, &count, [](void* context, const NativeOpenLotView&) {
                    ++*static_cast<std::size_t*>(context); return false;
                }) == Status::cancelled);
                CHECK(count == 1);
            }
        };
        auto configuration = spec("o1s2-ordinary-run");
        configuration.initial_capital = 100000000;
        run(host, configuration, {100, 100, 100});
        completed(host);
        CHECK(observations == 2);
    }
}
} // namespace

int main() {
    test("opening_input_and_readback_and_observations", openings_and_observations);
    test("core_group_children_and_bound_visitors", core_handle_visitors);
    test("ordinary_working_point_and_lot_visitors", ordinary_run_visitors);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
