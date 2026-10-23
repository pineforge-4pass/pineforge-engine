// CX-JOURNAL-CAP. Commit this TU and its lane registration before the fix.
// Static read only here; the supervisor supplies the RED and GREEN executions.
// Uses existing public values only; requires no declaration added by the fix.
#include "native_order_full_fold.hpp"

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = native_order;
int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                          \
} while (false)

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::size_t kCap = 2048;
constexpr std::size_t kChunk = 32;
constexpr std::size_t kOrders = 6000;
constexpr std::size_t kEvents = 3 * kOrders;

no::Request request(double units, double price) {
    no::Request result{no::Transact{units}, "cap", ""};
    result.trigger = no::Limit{price};
    return result;
}

enum class Phase { Accepted, Replaced, Cancelled };
struct Owners {
    std::weak_ptr<const no::RequestDefinition> original;
    std::weak_ptr<const no::RequestDefinition> successor;
    Phase phase = Phase::Accepted;
};

// No old event is read until all residency samples finish: decoded cache
// definitions must not enter this census. The only strong owners are the
// journal events and, for the newest order, its one live book row.
//
// A owns original; R owns original AND successor; C owns successor. Subtract
// the live row, then count R once. Because spills remove a prefix, original
// can survive after replacement iff R is resident (A is older than R).
// This counts events exactly, even when a page ends inside an A/R/C triple.
// Weak control blocks are test infrastructure, not a physical heap measurement.
std::size_t resident_events(const std::vector<Owners>& owners) {
    std::size_t result = 0;
    for (const auto& row : owners) {
        auto original = row.original.use_count();
        auto successor = row.successor.use_count();
        if (row.phase == Phase::Accepted) --original; // live original
        if (row.phase == Phase::Replaced) --successor; // live successor
        require(original >= 0 && original <= 2 && successor >= 0 && successor <= 2,
                "unexpected definition owner in resident census");
        const auto replacement = row.phase != Phase::Accepted && original != 0 ? 1 : 0;
        result += static_cast<std::size_t>(original + successor - replacement);
    }
    return result;
}

// No large-cap override exists. Build the receipt/state oracle from the
// commands' specified values, without reading any event from a reference core.
void expected_order(const no::RunIdentity& run, std::size_t index,
                    std::vector<std::uint64_t>& values, full_fold::Fold& state) {
    const auto ordinal = static_cast<std::uint64_t>(3 * index + 1);
    const no::RequestHandle first{run, static_cast<std::uint64_t>(2 * index + 1)};
    const no::RequestHandle second{run, first.incarnation + 1};
    auto original = std::make_shared<no::RequestDefinition>(
        no::RequestDefinition{first, request(1.0, 50.0), {ordinal, 0}, std::nullopt});
    auto successor = std::make_shared<no::RequestDefinition>(
        no::RequestDefinition{second, request(2.0, 60.0), {ordinal + 1, 0}, first});
    successor->root = first;
    no::CancelledEvent cancelled;
    cancelled.ordinal = ordinal + 2;
    cancelled.definition = successor;
    cancelled.reason = no::CancelReason::User;
    cancelled.cause = no::EventId{run, ordinal + 2};
    cancelled.prior_authority = no::BookTransaction{};
    cancelled.unexecuted = no::RemainingProjectionUnits{2.0};
    const no::CommandEvent events[] = {
        no::AcceptedEvent{ordinal, original},
        no::ReplacedEvent{ordinal + 1, original, successor},
        std::move(cancelled),
    };
    for (const auto& event : events) {
        values.push_back(full_fold::value(event));
        full_fold::fold(state, event);
        state.b(true); // event_at of the same ordinal finds this event
    }
}

void one_unretired_window(bool direct) {
    const no::RunIdentity run{"journal-cap", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1, ordinal = 1;
    std::vector<Owners> owners;
    std::vector<std::uint64_t> expected;
    owners.reserve(kOrders);
    expected.reserve(kEvents);
    full_fold::Fold state;
    full_fold::fold(state, run);
    state.u(kEvents);      // last ordinal
    state.u(2 * kOrders);  // last incarnation
    state.u(0);            // history base
    state.u(kEvents);      // history end
    state.u(0);            // retired through
    std::size_t peak = 0;
    std::size_t over_bound = 0;
    const auto sample = [&] {
        const auto resident = resident_events(owners);
        peak = std::max(peak, resident);
        if (resident > kCap + kChunk) ++over_bound;
        // Every sample is inside the same window, before any retirement.
        require(core.history_base() == 0, "the resident census must precede retirement");
    };

    for (std::size_t index = 0; index < kOrders; ++index) {
        no::SubmitResult submitted;
        if (direct) {
            submitted = core.apply_submit(request(1.0, 50.0), {}, incarnation, ordinal).result;
            ++incarnation;
            ++ordinal;
        } else {
            submitted = core.submit(request(1.0, 50.0), 0, incarnation, ordinal);
        }
        require(submitted.status == no::SubmitStatus::Accepted && submitted.handle,
                "accepted fixture request");
        owners.push_back({std::get<no::AcceptedEvent>(core.history().back()).definition,
                          {}, Phase::Accepted});
        sample();

        no::ReplaceResult replaced;
        if (direct) {
            replaced = core.apply_replace(*submitted.handle, request(2.0, 60.0), {},
                                          incarnation, ordinal).result;
            ++incarnation;
            ++ordinal;
        } else {
            replaced = core.replace(*submitted.handle, request(2.0, 60.0), 0,
                                    incarnation, ordinal);
        }
        require(replaced.status == no::ReplaceStatus::Replaced && replaced.successor,
                "replaced fixture request");
        owners.back().successor =
            std::get<no::ReplacedEvent>(core.history().back()).successor_definition;
        owners.back().phase = Phase::Replaced;
        sample();

        no::CancelResult cancelled;
        if (direct) {
            cancelled = core.apply_cancel(*replaced.successor, ordinal).result;
            ++ordinal;
        } else {
            cancelled = core.cancel(*replaced.successor, ordinal);
        }
        require(cancelled.status == no::CancelStatus::Cancelled, "cancelled fixture request");
        owners.back().phase = Phase::Cancelled;
        sample();
        expected_order(run, index, expected, state);
    }

    std::printf("journal_resident_cap direct=%d events=%zu peak_resident=%zu bound=%zu "
                "over_bound_samples=%zu\n", direct, core.history_end(), peak,
                kCap + kChunk, over_bound);
    // Old kMaxResident=16384 reaches exactly 16384 here; the limit is 2080.
    CHECK(over_bound == 0);
    CHECK(peak <= kCap + kChunk);
    CHECK(core.history_base() == 0 && core.history_end() == kEvents);
    CHECK(core.live().empty());
    CHECK(owners.front().original.expired() && owners.front().successor.expired());
    // The original owners are gone, but their unretired events still read
    // back: an external page was written, not just an empty chunk reserved.
    CHECK(full_fold::value(core.history_at(0)) == expected.front());
    for (std::size_t position = 0; position < expected.size(); ++position)
        CHECK(full_fold::value(core.history_at(position)) == expected[position]);

    state.u(0);            // live book
    state.u(0);            // group-effect receipts
    state.u(1);            // one dense issued range
    state.u(1);
    state.u(2 * kOrders);
    state.u(0);            // cohort rosters
    state.u(0);            // cohort-receipt base
    state.u(0);            // cohort-receipt count
    CHECK(full_fold::core_value(core) == state.h);
    CHECK(incarnation == 2 * kOrders + 1 && ordinal == kEvents + 1);
}

// Real settlements as well as cancellation receipts: 6000 market orders,
// 3000 round trips in one bar, with independently known trades (one unit at
// 100 -> 100, zero fees). No engine output supplies an expected trade.
constexpr std::size_t kTrades = kOrders / 2;
constexpr std::int64_t kStart = 1736121600000LL;
struct TradingHost final : NativeStrategyHost {
    std::size_t callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        for (std::size_t index = 0; index < kTrades; ++index) {
            execute(no::Request{no::Transact{1.0}, "open", ""}, index, true);
            execute(no::Request{no::Flatten{}, "close", ""}, index, false);
        }
    }
    void execute(const no::Request& input, std::size_t index, bool opening) {
        const auto accepted = submit(input);
        require(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
                "accepted market fixture request");
        const auto result = execute_current({*accepted.handle, NativeCurrentPriceRule::AsPresented});
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&result);
        require(applied != nullptr, "applied market fixture request");
        CHECK(applied->raw_price == 100.0 && applied->resolved_price == 100.0);
        CHECK(applied->opened_units == (opening ? 1.0 : 0.0));
        CHECK(applied->closed_units == (opening ? 0.0 : 1.0));
        CHECK(applied->closed_trade_count == (opening ? 0u : 1u));
        if (!opening) CHECK(applied->first_trade_index == index);
        CHECK(applied->terminal);
    }
    const TradeLedger& rows() const { return trades_; }
};

void same_bar_trades() {
    NativeRunSpec spec;
    spec.identity = {"journal-cap-trades", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:CAP";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    spec.event_retention = NativeEventRetention::Full;
    TradingHost host;
    require(host.configure_native(spec).status == NativeSetupStatus::Applied,
            "configured market fixture");
    const Bar bar{100, 100, 100, 100, 1, kStart};
    host.run(&bar, 1);
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.callbacks == 1 && host.rows().size() == kTrades);
    CHECK(host.physical_position().signed_units == 0.0);
    std::size_t index = 0;
    for (const auto& trade : host.rows()) {
        CHECK(trade.entry_time == kStart + 60000 && trade.exit_time == kStart + 60000);
        CHECK(trade.entry_bar_index == 0 && trade.exit_bar_index == 0);
        CHECK(trade.entry_price == 100.0 && trade.exit_price == 100.0);
        CHECK(trade.qty == 1.0 && trade.pnl == 0.0 && trade.pnl_pct == 0.0);
        CHECK(trade.is_long && !trade.open_at_end && !trade.exit_from_bracket);
        CHECK(trade.entry_id == "open" && trade.exit_id == "close");
        CHECK(trade.entry_comment.empty() && trade.exit_comment.empty());
        CHECK(trade.commission == 0.0 && trade.max_runup == 0.0 && trade.max_drawdown == 0.0);
        CHECK(trade.entry_incarnation == 2 * index + 1);
        CHECK(trade.close_cause == execution::CloseCause::Unspecified);
        ++index;
    }
    const auto before_readback = host.native_continuation_hash();
    std::size_t executions = 0;
    for (const auto& event : host.native_events(0)) {
        if (event.command && std::holds_alternative<no::ExecutionAppliedEvent>(*event.command))
            ++executions;
    }
    CHECK(executions == kOrders);
    CHECK(host.native_continuation_hash() == before_readback);
}
} // namespace

int main() {
    try {
        one_unretired_window(false);
        one_unretired_window(true);
        same_bar_trades();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "journal_resident_cap exception: %s\n", error.what());
    }
    std::printf("journal_resident_cap: %d failures\n", failures);
    return failures ? 1 : 0;
}
