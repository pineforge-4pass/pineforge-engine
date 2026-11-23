#include <pineforge/native_order.hpp>

#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace {
using namespace pineforge::native_order;
constexpr auto bad_surface = static_cast<CommandSurface>(7);
constexpr auto bad_origin = static_cast<RequestOrigin>(9);

// A missing witness ends the test with a nonzero exit, even in Release.
struct ComparisonFailed {};

template<class A, class B>
void compare(const char* status, const std::string& row, const A& actual, const B& expected) {
    if (actual == expected) return;
    std::cerr << status << ' ' << row << ": expected=" << expected << " actual=" << actual << '\n';
    throw ComparisonFailed{};
}

constexpr char setup[] = "NOT COMPARED";
constexpr char fail[] = "FAIL";

struct Book {
    WorkingRequestCore core{RunIdentity{"issue-974", 1}};
    std::uint64_t inc = 1, ord = 1;
    Request request{Transact{2}, "issue-974", ""};

    // Check the actual journal row against the result before using its definition.
    DefinitionRef accepted(const std::string& row, const CommandInstalled<SubmitResult>& cmd,
                           CommandSurface surface, RequestOrigin origin, const Request& input) {
        compare(setup, row + " retained events", core.history().size(), ord);
        compare(setup, row + " status", int(cmd.result.status), int(SubmitStatus::Accepted));
        compare(setup, row + " handle", cmd.result.handle.has_value(), true);
        compare(setup, row + " event count", cmd.events.count, 1u);
        compare(setup, row + " history end", core.history_end(), cmd.events.first_index + 1);
        const auto* event = std::get_if<AcceptedEvent>(&core.history().back());
        compare(setup, row + " AcceptedEvent", event != nullptr, true);
        compare(setup, row + " definition", bool(event->definition), true);
        compare(setup, row + " ordinal", event->ordinal, ord);
        compare(setup, row + " result ordinal", cmd.result.event_ordinal, ord);
        compare(setup, row + " result handle matches event", *cmd.result.handle == event->handle(), true);
        compare(setup, row + " incarnation", event->handle().incarnation, inc);
        compare(setup, row + " run matches", event->handle().run == core.identity(), true);
        compare(setup, row + " label", event->request().label, input.label);
        compare(setup, row + " working request", core.find_live(event->handle()) != nullptr, true);
        compare(setup, row + " surface", int(event->surface), int(surface));
        compare(setup, row + " origin", int(event->definition->origin), int(origin));
        ++inc;
        ++ord;
        return event->definition;
    }
};

void command_boundaries() {
    Book b;
    CommandContext ctx;
    const auto seed = b.accepted("seed", b.core.apply_submit(b.request, ctx, b.inc, b.ord),
                                 ctx.surface, RequestOrigin::Host, b.request);
    // One pending command is the witness that none of the invalid calls sealed a plan.
    ctx.surface = CommandSurface::MarketOnly;
    const Request next{Flatten{}, "risk flatten", ""};
    auto pending = b.core.prepare_submit(next, ctx, b.inc, b.ord, RequestOrigin::KernelRisk);
    compare(setup, "pending token", bool(pending), true);
    compare(setup, "pending status", int(pending.predicted().status), int(SubmitStatus::Accepted));

    auto refuse = [&](const std::string& row, auto command) {
        try {
            command();
        } catch (const std::invalid_argument&) {
            compare(fail, row + " history", b.core.history_end(), 1u);
            compare(fail, row + " live", b.core.live().size(), 1u);
            compare(fail, row + " original definition", b.core.live().front().definition == seed, true);
            compare(fail, row + " last incarnation", b.core.last_incarnation(), 1u);
            compare(fail, row + " last ordinal", b.core.last_ordinal(), 1u);
            compare(fail, row + " next incarnation", b.inc, 2u);
            compare(fail, row + " next ordinal", b.ord, 2u);
            return;
        }
        // Show the issue's actual values on the base, including what was committed.
        std::cerr << "FAIL " << row << ": expected=std::invalid_argument actual=returned";
        if (b.core.history_end() > 1 && !b.core.history().empty()) {
            if (const auto* event = std::get_if<AcceptedEvent>(&b.core.history().back())) {
                if (event->definition)
                    std::cerr << " AcceptedEvent surface=" << int(event->surface)
                              << " origin=" << int(event->definition->origin);
            }
        }
        std::cerr << '\n';
        throw ComparisonFailed{};
    };

    ctx.surface = bad_surface;
    refuse("apply_submit(7,9)", [&] { b.core.apply_submit(b.request, ctx, b.inc, b.ord, bad_origin); });
    for (bool direct : {false, true}) {
        const std::string submit = direct ? "apply_submit" : "prepare_submit";
        const std::string replace = direct ? "apply_replace" : "prepare_replace";
        auto request = b.request;
        request.trigger = Limit{100}; // valid on General, refused on MarketOnly
        ctx.surface = bad_surface;
        refuse(submit + "(7,Host)", [&] {
            if (direct) b.core.apply_submit(request, ctx, b.inc, b.ord);
            else b.core.prepare_submit(request, ctx, b.inc, b.ord);
        });
        auto bad_replace = [&](const RequestHandle& target, bool keep) {
            ReplaceOptions options;
            options.keep_handle = keep;
            refuse(replace + (keep ? "(7,keep) target=" : "(7) target=") +
                   std::to_string(target.incarnation), [&] {
                if (direct) b.core.apply_replace(target, request, ctx, b.inc, b.ord, options);
                else b.core.prepare_replace(target, request, ctx, b.inc, b.ord, options);
            });
        };
        bad_replace(seed->handle, false);
        bad_replace(seed->handle, true);
        bad_replace({b.core.identity(), 99}, false);
        bad_replace({}, false);
        ctx.surface = CommandSurface::General;
        refuse(submit + "(General,9)", [&] {
            if (direct) b.core.apply_submit(request, ctx, b.inc, b.ord, bad_origin);
            else b.core.prepare_submit(request, ctx, b.inc, b.ord, bad_origin);
        });
    }

    auto installed = b.core.install_submit(std::move(pending));
    const auto* ok = std::get_if<CommandInstalled<SubmitResult>>(&installed);
    compare(fail, "pending command installed", ok != nullptr, true);
    b.accepted("pending command", *ok, CommandSurface::MarketOnly, RequestOrigin::KernelRisk, next);
}

void margin_receipt_origin() {
    Book b;
    const Request close{Reduce{ExplicitUnits{2}}, "liquidation", ""};
    const auto origin = RequestOrigin::KernelLiquidation;
    const auto def = b.accepted("liquidation", b.core.apply_submit(close, {}, b.inc, b.ord, origin),
                                CommandSurface::General, origin, close);

    // Take the receipt from a real terminal close in this book. For the negative
    // cases, change only the origin in a copy of that definition.
    EvaluationContext point;
    point.cursor.point.ordinal = b.ord + 1;
    point.cursor.point.effective_time_ms = 1000;
    point.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    point.driver_class = DriverEligibilityClass::ObservedPrint;
    point.existing_matching_bit = true;
    TargetObservation book;
    book.current_position = PositionNonflat{1, Side::Long};
    const auto bound = b.core.apply_evaluation(def->handle, point, book, b.ord);
    compare(setup, "liquidation bound", std::holds_alternative<Installed>(bound), true);
    compare(setup, "bound event count", std::get<Installed>(bound).events.count, 1u);
    ++b.ord;
    ExecutionProposal proposal;
    proposal.cursor = point.cursor;
    proposal.raw_price = proposal.resolved_price = 100;
    proposal.physical_action = pineforge::order_action::Reduce{2};
    proposal.pre_target = book;
    proposal.pre_fill = book.current_position;
    proposal.inspected_closed_units = 2;
    proposal.inspected_current_ticket = 1;
    auto fill = b.core.prepare_execution(def->handle, proposal, b.ord);
    compare(setup, "prepared fill", std::holds_alternative<PreparedExecution>(fill), true);
    CommittedExecutionFacts facts;
    facts.result.status = pineforge::execution::Status::Applied;
    facts.result.closed_units = 2;
    facts.result.current_ticket = 1;
    facts.cycle_before = 1;
    facts.post_target.current_position = PositionFlat{};
    facts.committed_action = proposal.physical_action;
    const auto filled = b.core.install_execution(std::get<PreparedExecution>(std::move(fill)), facts);
    compare(setup, "installed fill", std::holds_alternative<Installed>(filled), true);
    compare(setup, "fill event count", std::get<Installed>(filled).events.count, 1u);
    const auto* applied = std::get_if<ExecutionAppliedEvent>(b.core.event_at({b.core.identity(), b.ord}));
    compare(setup, "applied event", applied != nullptr, true);
    compare(setup, "filled definition", applied->definition == def, true);
    compare(setup, "terminal fill", applied->terminal, true);
    compare(setup, "filled units", applied->closed_units, 2.0);
    ++b.ord;

    MarginCallEvent receipt;
    receipt.applied = {b.core.identity(), applied->ordinal};
    receipt.cursor = applied->cursor;
    receipt.mark = applied->resolved_price;
    receipt.units = receipt.position_before = applied->closed_units;
    const auto before = b.core.history_end();
    for (auto wrong : {bad_origin, RequestOrigin::KernelRisk, RequestOrigin::Host}) {
        auto copy = std::make_shared<RequestDefinition>(*def);
        copy->origin = wrong;
        receipt.definition = copy;
        auto refused = [&](const char* path, const auto& result) {
            const auto* error = std::get_if<PreparationError>(&result);
            const auto row = std::string(path) + " origin=" + std::to_string(int(wrong));
            compare(fail, row + " PreparationError", error != nullptr, true);
            compare(fail, row + " reason", int(error->code), int(CoreFailure::InvalidProposal));
            compare(fail, row + " target", error->target == def->handle, true);
            compare(fail, row + " cause", error->cause == receipt.applied, true);
            compare(fail, row + " history", b.core.history_end(), before);
            compare(fail, row + " ordinal", b.core.last_ordinal(), receipt.applied.ordinal);
        };
        refused("prepare_margin_call", b.core.prepare_margin_call(receipt, b.ord));
        refused("apply_margin_call", b.core.apply_margin_call(receipt, b.ord));
    }
    receipt.definition = def;
    const auto result = b.core.apply_margin_call(receipt, b.ord);
    compare(fail, "liquidation receipt installed", std::holds_alternative<Installed>(result), true);
    compare(fail, "receipt count", std::get<Installed>(result).events.count, 1u);
    const auto* event = std::get_if<MarginCallEvent>(b.core.event_at({b.core.identity(), b.ord}));
    compare(fail, "margin event", event != nullptr, true);
    compare(fail, "margin definition", event->definition == def, true);
    compare(fail, "margin cause", event->applied == receipt.applied, true);
    compare(fail, "margin units", event->units, 2.0);
}
} // namespace

int main() {
    try {
        command_boundaries();
        margin_receipt_origin();
    } catch (const ComparisonFailed&) {
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "NOT COMPARED fixture: expected=completed comparison actual=" << e.what() << '\n';
        return 1;
    }
    std::cout << "PASS issue-974: command enums and margin receipt origin\n";
}
