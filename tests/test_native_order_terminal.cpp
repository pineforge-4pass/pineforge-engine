#include <pineforge/native_order.hpp>

#include <cstdio>
#include <string>

using pineforge::execution::Flatten;
using pineforge::native_order::CommandEvent;
using pineforge::native_order::ExecutionAppliedEvent;
using pineforge::native_order::MatchRejectedEvent;
using pineforge::native_order::NoEffectEvent;
using pineforge::native_order::Request;
using pineforge::native_order::RunIdentity;
using pineforge::native_order::TerminalCommit;
using pineforge::native_order::WorkingRequestCore;
using pineforge::order_action::Transact;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
        }                                                               \
    } while (0)
}  // namespace

int main() {
    RunIdentity id{"terminal-session", 1};
    WorkingRequestCore core(id);
    uint64_t incarnation = 1;
    uint64_t ordinal = 1;
    auto accepted = core.submit(Request{Transact{1.0}, "buy", ""}, 60000, incarnation, ordinal);
    CHECK(accepted.status == pineforge::native_order::SubmitStatus::Accepted);
    CHECK(core.live().size() == 1);
    CHECK(core.history().size() == 1);

    const uint64_t next = TerminalCommit::usable_ordinal(core, ordinal);
    CHECK(next == ordinal);
    TerminalCommit::reserve_history(core);
    NoEffectEvent none;
    none.ordinal = next;
    none.handle = *accepted.handle;
    none.request = Request{Transact{1.0}, "buy", ""};
    none.birth = core.live()[0].birth;
    TerminalCommit::install(core, 0, CommandEvent{std::move(none)});
    ++ordinal;
    CHECK(core.live().empty());
    CHECK(core.history().size() == 2);
    CHECK(std::holds_alternative<NoEffectEvent>(core.history().back()));

    auto second = core.submit(Request{Transact{-1.0}, "sell", ""}, 120000, incarnation, ordinal);
    CHECK(second.status == pineforge::native_order::SubmitStatus::Accepted);
    TerminalCommit::usable_ordinal(core, ordinal);
    TerminalCommit::reserve_history(core);
    MatchRejectedEvent rejected;
    rejected.ordinal = ordinal;
    rejected.handle = *second.handle;
    rejected.request = Request{Transact{-1.0}, "sell", ""};
    rejected.birth = core.live()[0].birth;
    rejected.reason = pineforge::native_order::MatchRejectReason::MaxAbsUnits;
    TerminalCommit::install(core, 0, CommandEvent{std::move(rejected)});
    ++ordinal;
    CHECK(core.live().empty());
    CHECK(std::holds_alternative<MatchRejectedEvent>(core.history().back()));

    auto third = core.submit(Request{Flatten{}, "flat", ""}, 180000, incarnation, ordinal);
    CHECK(third.status == pineforge::native_order::SubmitStatus::Accepted);
    TerminalCommit::usable_ordinal(core, ordinal);
    TerminalCommit::reserve_history(core);
    ExecutionAppliedEvent applied;
    applied.ordinal = ordinal;
    applied.handle = *third.handle;
    applied.request = Request{Flatten{}, "flat", ""};
    applied.birth = core.live()[0].birth;
    applied.resolved_price = 104;
    applied.current_ticket = 6;
    applied.first_trade_index = 0;
    applied.closed_trade_count = 1;
    TerminalCommit::install(core, 0, CommandEvent{std::move(applied)});
    CHECK(core.live().empty());
    CHECK(std::holds_alternative<ExecutionAppliedEvent>(core.history().back()));
    CHECK(std::get<ExecutionAppliedEvent>(core.history().back()).current_ticket == 6);

    std::printf("%s %d checks %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
