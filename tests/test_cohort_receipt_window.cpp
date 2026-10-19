// CR-150 L19C: a cohort receipt is retained only until its continuation fold.
// The first test commit uses only the base's APIs: it must fail because the
// production Window consumer retains every folded row, not for a missing API.
// Execute the red/green pair in the lab; nothing was compiled on the Mac.
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                           \
    }                                                                         \
} while (0)

constexpr std::int64_t kStart = 1736121600000LL;

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    }
    return bars;
}

NativeRunSpec spec() {
    NativeRunSpec out;
    out.identity = {"cohort-receipt-window", 1};
    out.input_tf = "1";
    out.script_tf = "1";
    out.tickerid = "TEST:COHORT";
    out.timezone = "UTC";
    out.session = "24x7";
    out.initial_capital = 100000.0;
    out.point_value = 1.0;
    out.account_fx = 1.0;
    out.price_tick = 0.25;
    return out;
}

class Host final : public NativeStrategyHost {
public:
    explicit Host(NativeEventRetention retention) : retention_(retention) {
        // Keep the spec and its hash identical; vary only readback retention,
        // exactly as the existing journal-window differential does.
        if (retention != NativeEventRetention::Window) {
            as_native_consumer(execution_consumer()).set_retention_override(retention);
        }
    }

    std::vector<std::uint64_t> trace;
    std::size_t commands = 0;
    std::size_t max_retained = 0;
    std::size_t max_capacity = 0;

    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }

    void on_native_run_begin() override {
        cohort_.reset();
        commands = max_retained = max_capacity = 0;
        trace.clear();
        CHECK(core().cohort_receipts().empty());
        CHECK(core().cohort_receipt_base() == 0);
        CHECK(core().cohort_receipt_count() == 0);
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (!cohort_) cohort_ = cohort_open();
        no::Request resting{no::Transact{1.0}, "resting", ""};
        resting.trigger = no::Limit{50.0};
        const auto accepted = submit(resting);
        CHECK(accepted.status == no::SubmitStatus::Accepted && accepted.handle);
        if (!accepted.handle) return;
        const auto live = *accepted.handle;
        const no::RequestHandle unknown{live.run, live.incarnation + 1000000};
        add(*cohort_, live, no::CohortReceiptStatus::Applied);
        add(*cohort_, live, no::CohortReceiptStatus::Applied);
        CHECK(core().cohort_contains(*cohort_, live));
        remove(*cohort_, live, no::CohortReceiptStatus::Applied);
        remove(*cohort_, live, no::CohortReceiptStatus::Applied);
        CHECK(!core().cohort_contains(*cohort_, live));
        add(no::CohortHandle{}, live, no::CohortReceiptStatus::InvalidHandle);
        remove(no::CohortHandle{}, live, no::CohortReceiptStatus::InvalidHandle);
        add(*cohort_, unknown, no::CohortReceiptStatus::UnknownOrigin);
        remove(*cohort_, unknown, no::CohortReceiptStatus::UnknownOrigin);
        CHECK(cancel(live).status == no::CancelStatus::Cancelled);
        add(*cohort_, live, no::CohortReceiptStatus::TerminalOrigin);
        remove(*cohort_, live, no::CohortReceiptStatus::Applied);

        // Exercise real settlements and closed rows in the same differential.
        no::Request market;
        if (physical_position().signed_units == 0.0) market.intent = no::Transact{1.0};
        else market.intent = no::Flatten{};
        CHECK(submit_market(market).status == no::SubmitStatus::Accepted);
    }

private:
    void observe(no::CohortReceiptOperation operation, no::CohortHandle cohort,
                 const no::RequestHandle& origin, no::CohortReceiptStatus status) {
        ++commands;
        const auto& rows = core().cohort_receipts();
        CHECK(core().cohort_receipt_count() == commands);
        max_retained = std::max(max_retained, rows.size());
        max_capacity = std::max(max_capacity, rows.capacity());
        if (retention_ == NativeEventRetention::Window) {
            CHECK(rows.empty());
            CHECK(core().cohort_receipt_base() == commands);
        } else {
            CHECK(core().cohort_receipt_base() == 0);
            CHECK(rows.size() == commands);
            if (!rows.empty()) {
                const auto& receipt = rows.back();
                CHECK(receipt.operation == operation && receipt.status == status
                      && receipt.cohort == cohort && receipt.origin == origin);
            }
        }
        // In Debug this also executes the consumer's total-count invariant
        // after every retirement, including every rejected roster command.
        trace.push_back(native_continuation_hash());
        trace.push_back(broker_state_hash());
    }

    void add(no::CohortHandle cohort, const no::RequestHandle& origin,
             no::CohortReceiptStatus status) {
        cohort_add(cohort, origin);
        observe(no::CohortReceiptOperation::Add, cohort, origin, status);
    }
    void remove(no::CohortHandle cohort, const no::RequestHandle& origin,
                no::CohortReceiptStatus status) {
        cohort_remove(cohort, origin);
        observe(no::CohortReceiptOperation::Remove, cohort, origin, status);
    }

    NativeEventRetention retention_;
    std::optional<no::CohortHandle> cohort_;
};

struct Result {
    std::vector<std::uint64_t> trace;
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
    std::size_t commands = 0;
    std::size_t retained = 0;
    std::size_t capacity = 0;
    long trades = 0;
};

Result run(Host& host, const std::vector<Bar>& bars, std::uint64_t generation = 1) {
    auto setup = spec();
    setup.identity.run_number = generation;
    CHECK(host.configure_native(setup).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.commands == 10 * bars.size());
    CHECK(host.trade_count() > 0);
    return {host.trace, host.native_continuation_hash(), host.broker_state_hash(),
            host.commands, host.max_retained, host.max_capacity, host.trade_count()};
}

Result differential(int count) {
    const auto bars = tape(count);
    Host window(NativeEventRetention::Window);
    Host full(NativeEventRetention::Full);
    Host commands(NativeEventRetention::Commands);
    const auto actual = run(window, bars);
    const auto reference = run(full, bars);
    const auto command_reference = run(commands, bars);
    for (const auto* expected : {&reference, &command_reference}) {
        CHECK(actual.trace == expected->trace);
        CHECK(actual.continuation == expected->continuation);
        CHECK(actual.broker == expected->broker);
        CHECK(actual.commands == expected->commands);
        CHECK(actual.trades == expected->trades);
        CHECK(expected->retained == expected->commands);
    }
    CHECK(actual.retained == 0);
    CHECK(actual.capacity <= 1);
    // A second begin on the same consumer must restart the fold and the base.
    const auto again = run(window, bars, 2);
    CHECK(again.trace == actual.trace);
    CHECK(again.continuation == actual.continuation);
    CHECK(again.broker == actual.broker);
    CHECK(again.retained == 0);
    CHECK(again.capacity <= 1);
    return actual;
}

bool same(const no::CohortReceipt& a, const no::CohortReceipt& b) {
    return a.operation == b.operation && a.status == b.status
        && a.cohort == b.cohort && a.origin == b.origin;
}

void window_mechanics() {
    const no::RunIdentity identity{"cohort-window-with-a-non-SSO-session-key", 1};
    no::WorkingRequestCore core(identity);
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    no::Request request{no::Transact{1.0}, "resting", ""};
    request.trigger = no::Limit{50.0};
    const auto accepted = core.submit(request, kStart, incarnation, ordinal);
    CHECK(accepted.status == no::SubmitStatus::Accepted && accepted.handle);
    if (!accepted.handle) return;
    const auto origin = *accepted.handle;
    const auto cohort = core.cohort_open();
    core.cohort_add(cohort, origin);
    core.cohort_add(no::CohortHandle{}, origin);
    core.cohort_remove(cohort, origin);
    core.cohort_add(cohort, origin);
    const auto rows = core.cohort_receipts();
    CHECK(rows.size() == 4 && core.cohort_receipt_base() == 0);
    CHECK(core.cohort_receipt_count() == 4);
    const auto last_ordinal = core.last_ordinal();
    const auto last_incarnation = core.last_incarnation();
    const auto history_end = core.history_end();
    CHECK(core.retire_cohort_receipts(2) == 2);
    CHECK(core.cohort_receipt_base() == 2 && core.cohort_receipt_count() == 4);
    CHECK(core.cohort_receipts().size() == 2);
    if (core.cohort_receipts().size() == 2) {
        CHECK(same(core.cohort_receipts()[0], rows[2]));
        CHECK(same(core.cohort_receipts()[1], rows[3]));
    }
    CHECK(core.retire_cohort_receipts(2) == 0);
    CHECK(core.retire_cohort_receipts(0) == 0);
    CHECK(core.last_ordinal() == last_ordinal);
    CHECK(core.last_incarnation() == last_incarnation);
    CHECK(core.history_end() == history_end);
    CHECK(core.cohort_contains(cohort, origin));
    core.cohort_remove(cohort, origin);
    CHECK(!core.cohort_contains(cohort, origin));
    CHECK(core.cohort_receipt_base() == 2 && core.cohort_receipt_count() == 5);
    CHECK(core.cohort_receipts().back().operation == no::CohortReceiptOperation::Remove);
    CHECK(core.retire_cohort_receipts(std::numeric_limits<std::size_t>::max()) == 3);
    CHECK(core.cohort_receipts().empty());
    CHECK(core.cohort_receipt_base() == 5 && core.cohort_receipt_count() == 5);
    core.cohort_add(cohort, origin);
    CHECK(core.cohort_receipt_base() == 5 && core.cohort_receipt_count() == 6);
    CHECK(core.cohort_receipts().size() == 1);
    CHECK(core.cohort_receipts().back().status == no::CohortReceiptStatus::Applied);

    no::WorkingRequestCore moved(std::move(core));
    CHECK(moved.cohort_receipt_base() == 5 && moved.cohort_receipt_count() == 6);
    CHECK(moved.cohort_receipts().size() == 1 && moved.cohort_contains(cohort, origin));
    CHECK(core.cohort_receipt_base() == 0 && core.cohort_receipt_count() == 0);
    CHECK(core.cohort_receipts().empty());
    no::WorkingRequestCore assigned({"discarded-run", 7});
    assigned.cohort_add(no::CohortHandle{}, no::RequestHandle{});
    assigned = std::move(moved);
    CHECK(assigned.cohort_receipt_base() == 5 && assigned.cohort_receipt_count() == 6);
    CHECK(assigned.cohort_receipts().size() == 1 && assigned.cohort_contains(cohort, origin));
    CHECK(moved.cohort_receipt_base() == 0 && moved.cohort_receipt_count() == 0);
    CHECK(moved.cohort_receipts().empty());
    auto* alias = &assigned;
    assigned = std::move(*alias);
    CHECK(assigned.cohort_receipt_base() == 5 && assigned.cohort_receipt_count() == 6);
    CHECK(assigned.retire_cohort_receipts(6) == 1);
    CHECK(assigned.retire_cohort_receipts(6) == 0);
    assigned.reset({"reset-run", 2});
    CHECK(assigned.cohort_receipt_base() == 0 && assigned.cohort_receipt_count() == 0);
    CHECK(assigned.cohort_receipts().empty() && assigned.cohorts().empty());
    CHECK(assigned.cohort_open().value == 1);
}
}  // namespace

int main() {
    const auto small = differential(32);
    const auto large = differential(128);
    CHECK(large.commands == 4 * small.commands);
    CHECK(large.capacity == small.capacity);
    window_mechanics();
    std::printf("test_cohort_receipt_window: %d checks, %d failures; "
                "%zu/%zu commands, %zu/%zu retained capacity\n",
                checks, failures, small.commands, large.commands, small.capacity, large.capacity);
    return failures == 0 ? 0 : 1;
}
