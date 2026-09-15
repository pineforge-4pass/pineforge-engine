// A32 native witnesses for the generic settlement ticket handoff and exact
// selected-opening reduction arithmetic. No source adapter participates.
#include "native_current_fixture.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace r4_test;

namespace {

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "binary64 width");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

constexpr double kCashPerOrder = 5.1494;
constexpr double kInspectedTicket = 5.1494000000000009;
constexpr double kRecomputedTicket = 5.1494000000000018;
constexpr std::array<double, 3> kTicketOpeningUnits{0.0001, 0.0001, 0.001};
constexpr double kTicketClosedUnits =
    (kTicketOpeningUnits[0] + kTicketOpeningUnits[1]) + kTicketOpeningUnits[2];

class TicketHost final : public Host {
public:
    mutable bool saw_close_precommit = false;
    mutable double precommit_inspected_ticket = 0.0;
    mutable double precommit_preview_ticket = 0.0;

    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        if (view.definition && view.definition->request.label == "ticket-close") {
            saw_close_precommit = true;
            precommit_inspected_ticket = view.inspected_current_ticket;
            precommit_preview_ticket = view.account.current_ticket;
        }
        return NativePrecommitVerdict::Proceed;
    }
};

void inspected_ticket_is_installed_bitwise() {
    TicketHost host;
    NativeCurrentExecutionPreview preview;
    NativeCurrentExecutionResult executed = NativeCurrentRefusal::NoExecutionContext;
    host.calculation = [&](Host& base) {
        if (base.calculations != 1) return;
        std::vector<no::RequestHandle> openings;
        for (const double units : kTicketOpeningUnits) {
            const auto opening = put(base, tx(units, "ticket-opening"));
            apply(base, opening);
            openings.push_back(opening);
        }
        auto close = flat("ticket-close");
        close.owner = no::BindOpenings{openings, base.cycle()};
        const auto handle = put(base, close);
        preview = base.inspect_current_execution(command(handle));
        executed = base.execute_current(command(handle));
    };

    run(host, spec("l4h-ticket", kCashPerOrder), {100.0});

    CHECK(bits(kInspectedTicket) != bits(kRecomputedTicket));
    CHECK(!preview.refusal);
    CHECK(preview.settlement_readiness == ex::Status::Applied);
    CHECK(bits(preview.account.current_ticket) == bits(kInspectedTicket));
    CHECK(host.saw_close_precommit);
    CHECK(bits(host.precommit_inspected_ticket) == bits(kInspectedTicket));
    CHECK(bits(host.precommit_preview_ticket) == bits(kInspectedTicket));
    CHECK(std::holds_alternative<no::ExecutionAppliedEvent>(executed));
    if (const auto* event = std::get_if<no::ExecutionAppliedEvent>(&executed)) {
        CHECK(bits(event->current_ticket) == bits(kInspectedTicket));
        CHECK(bits(event->current_ticket) == bits(preview.account.current_ticket));
        CHECK(event->closed_units == kTicketClosedUnits);
        CHECK(event->closed_trade_count == 3);
    }
    CHECK(host.physical_position().signed_units == 0.0);
    completed(host);
}

constexpr double kDustOpening = 0x1p-48;
constexpr double kOpeningA = 0x1.f5905cf7f98a2p+4;
constexpr double kOpeningB = 0x1.f5905cf7f98a1p+4;
constexpr double kSelectedExposure = (kDustOpening + kOpeningA) + kOpeningB;
static_assert(kSelectedExposure == 0x1.f5905cf7f98a2p+5,
              "H07 selected exposure literal");

void stop_reduction_consumes_exact_selected_openings() {
    Host host;
    no::RequestHandle stop;
    host.calculation = [&](Host& base) {
        if (base.calculations != 1) return;
        std::vector<no::RequestHandle> openings;
        for (const double units :
             std::array<double, 3>{kDustOpening, kOpeningA, kOpeningB}) {
            const auto opening = put(base, tx(units, "selected-opening"));
            apply(base, opening);
            openings.push_back(opening);
        }
        CHECK(base.physical_position().signed_units == kSelectedExposure);
        auto reduction = reduce(kSelectedExposure, "selected-stop");
        reduction.owner = no::BindOpenings{openings, base.cycle()};
        reduction.trigger = no::Stop{99.0};
        stop = put(base, reduction);
    };

    REQUIRE(host.configure_native(spec("l4h-selected-stop")).status
            == NativeSetupStatus::Applied);
    const std::array<Bar, 2> bars{{
        {100.0, 100.0, 100.0, 100.0, 1.0, T},
        {100.0, 100.0, 98.0, 98.0, 1.0, T + 60000},
    }};
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(kSelectedExposure == 62.695489823630183);
    CHECK(host.physical_position().signed_units == 0.0);
    CHECK(host.rows().size() == 3);
    const auto applied = events<no::ExecutionAppliedEvent>(host);
    const auto found = std::find_if(applied.begin(), applied.end(),
        [&](const no::ExecutionAppliedEvent& event) { return event.handle() == stop; });
    CHECK(found != applied.end());
    if (found != applied.end()) {
        CHECK(found->closed_units == kSelectedExposure);
        CHECK(found->closed_trade_count == 3);
        CHECK(found->terminal);
    }
    completed(host);
}

} // namespace

int main() {
    test("inspected ticket is installed bitwise", inspected_ticket_is_installed_bitwise);
    test("Stop consumes exact selected openings",
         stop_reduction_consumes_exact_selected_openings);
    std::printf("L4h native settlement arithmetic: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
