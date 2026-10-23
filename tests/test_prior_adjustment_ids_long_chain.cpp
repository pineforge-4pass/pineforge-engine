// B1-12: contract regression coverage for the documented inline/paged split.
// Runtime behavior is already correct on the base; the field documentation
// is the fix. A mutation that drops paged visitation must fail this row.
#include "p2_kernel_order_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <variant>
#include <vector>

namespace {
namespace no = pineforge::native_order;
using p2_kernel_order_test::Fixture;
using p2_kernel_order_test::require;

std::vector<no::EventId> append_chain(Fixture& fixture, const no::RequestHandle& target,
                                    std::size_t count) {
    std::vector<no::EventId> expected;
    for (std::size_t i = 0; i != count; ++i) {
        no::Request request{no::Transact{1.0}, "adjustment", ""};
        request.group = no::Member{31, 1, no::GroupEffect::Reduce};
        const auto source = fixture.submit(request);
        const auto cause = fixture.fill(source, 1.0);
        // The caller supplies the next ordinal. Pin it before the operation,
        // independently of the receipt that will be read back.
        const no::EventId id{fixture.run, fixture.ordinal};
        fixture.mutation(fixture.core.prepare_group_effect(cause, target, fixture.ordinal));
        const auto* event = fixture.core.event_at(id);
        const auto* receipt = event ? std::get_if<no::DeferredGroupAdjustmentEvent>(event) : nullptr;
        require(receipt && receipt->ordinal == id.ordinal && receipt->deferred_delta == 1.0
                    && receipt->recipient == target && receipt->pending_after.count == i + 1
                    && receipt->pending_after.total == static_cast<double>(i + 1),
                "test did not build the requested real adjustment chain");
        expected.push_back(id);
    }
    return expected;
}

void check_ids(Fixture& fixture, const no::EventId& resolution,
               const std::vector<no::EventId>& expected,
               const std::vector<no::EventId>& inline_ids) {
    require(no::WorkingRequestCore::kInlineAdjustmentIds == 256,
            "documented 256-link cutoff changed");
    if (expected.size() <= 256)
        require(inline_ids == expected, "short chain does not carry all inline ids in order");
    else
        require(inline_ids.empty(), "long chain unexpectedly materialized inline ids");
    require(fixture.core.prior_adjustment_count(resolution) == expected.size(),
            "resolution's complete prior-adjustment count is wrong");
    std::vector<no::EventId> visited;
    require(fixture.core.visit_prior_adjustments(resolution, [&](const no::EventId& id) {
        visited.push_back(id);
        const auto* event = fixture.core.event_at(id); // Legal nested journal read.
        require(event && std::holds_alternative<no::DeferredGroupAdjustmentEvent>(*event),
                "prior id does not name its retained adjustment");
        return true;
    }), "resolution does not expose its prior ids");
    require(visited == expected, "complete prior-id visitation is not in chain order");

    std::size_t stopped = 0;
    require(fixture.core.visit_prior_adjustments(resolution, [&](const no::EventId& id) {
        require(stopped < expected.size() && id == expected[stopped],
                "short visitation changed chain order");
        return ++stopped != 7;
    }), "early-stopped resolution was not recognized");
    require(stopped == 7, "visitor ignored its stop request");
}

void check_after_spill(Fixture& fixture, const no::EventId& resolution,
                       const std::vector<no::EventId>& expected) {
    for (std::size_t i = 0; i != 2048 + 32; ++i) fixture.reject_small();
    const auto* event = fixture.core.event_at(resolution);
    require(event != nullptr, "resolution was lost after spill");
    // Own the small inline vector before check_ids performs nested page reads.
    std::vector<no::EventId> inline_ids;
    if (const auto* terms = std::get_if<no::TermsResolvedEvent>(event))
        inline_ids = terms->prior_adjustment_ids;
    else if (const auto* bound = std::get_if<no::QuantityBoundEvent>(event))
        inline_ids = bound->prior_adjustment_ids;
    else
        require(false, "spilled resolution has the wrong event type");
    check_ids(fixture, resolution, expected, inline_ids);
}

void terms_chain(std::size_t count) {
    Fixture fixture("p2-b1-12-terms");
    no::Request recipient{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "sized", ""};
    recipient.group = no::Member{31, 2, no::GroupEffect::Reduce};
    const auto target = fixture.submit(recipient);
    const auto expected = append_chain(fixture, target, count);
    const auto context = fixture.evaluate(target);
    no::TermsResolvedInput input;
    input.price_kind = no::NativeCandidatePriceKind::PointPrice;
    input.raw_price = input.default_resolved_price = input.terms.resolved_price = 100.0;
    input.terms.units = static_cast<double>(count + 10);
    input.terms.shape = no::OpeningShape::Transact;
    const no::EventId resolution{fixture.run, fixture.ordinal};
    fixture.mutation(fixture.core.prepare_terms(target, context, input, fixture.ordinal));
    {
        const auto* event = fixture.core.event_at(resolution);
        const auto* receipt = event ? std::get_if<no::TermsResolvedEvent>(event) : nullptr;
        require(receipt && receipt->pending_total == static_cast<double>(count)
                    && receipt->effective_deduction == static_cast<double>(count),
                "terms did not spend the full pending total");
        const auto* after = std::get_if<no::RemainingProjectionUnits>(&receipt->remaining_after);
        require(after && after->q == 10.0, "terms changed the remaining units");
        const auto inline_ids = receipt->prior_adjustment_ids;
        check_ids(fixture, resolution, expected, inline_ids);
    }
    check_after_spill(fixture, resolution, expected);
}

void quantity_chain(std::size_t count) {
    Fixture fixture("p2-b1-12-quantity");
    no::Request parent_request{no::Transact{static_cast<double>(count + 10)}, "parent", ""};
    parent_request.trigger = no::Limit{50.0};
    const auto parent = fixture.submit(parent_request);
    no::Request child_request{no::Reduce{no::OwnerOpenedUnits{}}, "child", ""};
    child_request.owner = no::WaitForApplied{parent};
    child_request.group = no::Member{31, 2, no::GroupEffect::Reduce};
    const auto target = fixture.submit(child_request);
    const auto expected = append_chain(fixture, target, count);
    const auto cause = fixture.fill(parent, static_cast<double>(count + 10), 50.0);
    const no::OpeningObservation opening{parent, 7, no::PositionNonflat{7, no::Side::Long}, true};
    const no::EventId resolution{fixture.run, fixture.ordinal};
    fixture.mutation(fixture.core.prepare_owner_applied(cause, target, opening, fixture.ordinal));
    {
        const auto* event = fixture.core.event_at(resolution);
        const auto* receipt = event ? std::get_if<no::QuantityBoundEvent>(event) : nullptr;
        require(receipt && receipt->source_units == static_cast<double>(count + 10)
                    && receipt->pending_total == static_cast<double>(count)
                    && receipt->effective_deduction == static_cast<double>(count),
                "quantity binding did not spend the full pending total");
        const auto* after = std::get_if<no::RemainingProjectionUnits>(&receipt->remaining);
        require(after && after->q == 10.0, "quantity binding changed remaining units");
        const auto inline_ids = receipt->prior_adjustment_ids;
        check_ids(fixture, resolution, expected, inline_ids);
    }
    check_after_spill(fixture, resolution, expected);
}
} // namespace

int main() {
    try {
        for (const std::size_t count : {std::size_t{256}, std::size_t{257}, std::size_t{300}}) {
            terms_chain(count);
            quantity_chain(count);
        }
        std::puts("test_prior_adjustment_ids_long_chain: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_prior_adjustment_ids_long_chain: FAIL: %s\n", error.what());
        return 1;
    }
}
