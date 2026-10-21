// CR-150 o1-e3: the consumer's per-run inventory family (REPORT A3 rows
// E06-E09, E11, E12). Every sequence below used to be a resident vector whose
// length was the run's -- B driver points, E fills, J commands, Q working
// requests -- so each row's witness must reach a population PAST the fixed
// staging of its store, show that the population spilled, and show that the
// resident scratch did not grow with it. Content and order are checked against
// an oracle the change did not write (the host's own rows, an independently
// replayed expected list, or the full rescan the matcher's row reuse replaces).
//
// Nothing here is a constant answer: every assertion reads a value the run
// produced (a count, a fold, a spill witness) and compares it with a second,
// independent computation of the same value.
#include "../src/native_execution_consumer.hpp"
#include "native_match_book_fixture.hpp"
#include "native_event_rows_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                       \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                           \
    }                                                                         \
} while (0)

constexpr std::int64_t kT0 = 1736121600000LL;
constexpr std::int64_t kMinute = 60000LL;

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        bars.push_back({100.0, 105.0, 95.0, 100.0, 1.0, kT0 + i * kMinute});
    }
    return bars;
}

NativeRunSpec spec(std::uint64_t run_number) {
    NativeRunSpec out;
    out.identity = {"o1e3-pages", run_number};
    out.input_tf = "1";
    out.script_tf = "1";
    out.tickerid = "TEST:O1E3";
    out.timezone = "UTC";
    out.session = "24x7";
    out.initial_capital = 1.0e9;
    out.point_value = 1.0;
    out.account_fx = 1.0;
    out.price_tick = 0.25;
    out.event_retention = NativeEventRetention::Full;
    return out;
}

// ---------------------------------------------------------------------------
// E07/E08/E09: the retained driver log and account log, and the merged read of
// all three event sources. The k3 book is randomized but seeded, so its rows
// are values the run produced, not constants this file picked.

void event_readback_rows(int width) {
    k3_book::BookConfig config;
    config.seed = 221;
    config.live = width;
    config.bars = 40;
    config.path = k3_book::Path::Synthesized;
    config.calc_on_fills = true;
    k3_book::BookHost host(config);
    const auto outcome = k3_book::run_book(host, config);
    CHECK(outcome.completed);
    CHECK(outcome.applied > 0 && outcome.accepted >= width);

    const auto& consumer = NativeExecutionConsumer::bound(host);
    // The oracle: the owning compatibility readback, walked in place.
    const auto expected = host.native_events(0);
    event_rows::Fold reference;
    std::size_t drivers = 0;
    std::size_t accounts = 0;
    for (const auto& row : expected) {
        if (row.kind == NativeEventKind::Driver) ++drivers;
        if (row.kind == NativeEventKind::Account) ++accounts;
        event_rows::fold_row(reference, row);
    }
    // The population is past the driver/account stores' staging tail: the
    // witness below is about a run that spilled, not about a small book.
    CHECK(drivers > 64);
    CHECK(accounts > 64);

    // E09: the paged read delivers every row, in order, with the same content.
    event_rows::Fold actual;
    std::size_t delivered = 0;
    NativeEventKind last_kind = NativeEventKind::Command;
    std::uint64_t last_ordinal = 0;
    bool ordered = true;
    consumer.visit_events_after(0, [&](const NativeMarketEvent& row) {
        const auto ordinal = row.ordinal;
        const auto kind = row.kind;
        event_rows::fold_row(actual, row);
        ordered = ordered
            && (delivered == 0 || ordinal > last_ordinal
                || (ordinal == last_ordinal
                    && static_cast<int>(kind) >= static_cast<int>(last_kind)));
        last_ordinal = ordinal;
        last_kind = kind;
        // Re-enter the whole merge while the outer row is still held: the
        // cursors are the call's own, so the nested read must see everything
        // and must not disturb the row being folded outside it.
        if ((delivered % 127) == 0) {
            std::size_t nested = 0;
            consumer.visit_events_after(0, [&](const NativeMarketEvent& inner) {
                ++nested;
                return false;
            });
            CHECK(nested == expected.size());
            CHECK(row.ordinal == ordinal && row.kind == kind);
        }
        ++delivered;
        return false;
    });
    CHECK(delivered == expected.size());
    CHECK(ordered);
    CHECK(actual.h == reference.h);

    // A cursor inside the stream returns exactly the oracle's suffix, so the
    // bisection into the paged stores picks the same first row.
    const std::uint64_t middle = expected.size() / 2;
    const std::uint64_t cursor = expected[middle].ordinal;
    event_rows::Fold suffix;
    std::size_t suffix_rows = 0;
    event_rows::Fold suffix_reference;
    std::size_t suffix_expected = 0;
    for (const auto& row : expected) {
        if (row.ordinal <= cursor) continue;
        event_rows::fold_row(suffix_reference, row);
        ++suffix_expected;
    }
    consumer.visit_events_after(cursor, [&](const NativeMarketEvent& row) {
        event_rows::fold_row(suffix, row);
        ++suffix_rows;
        return false;
    });
    CHECK(suffix_rows == suffix_expected);
    CHECK(suffix.h == suffix_reference.h);

    // E07/E08: the population is retained, and it is in the stores -- not in
    // the consumer's resident rows.
    const auto inventory = consumer.resident_inventory();
    CHECK(inventory.driver_rows == drivers);
    CHECK(inventory.account_rows == accounts);
    CHECK(inventory.spilled_rows > 0);
    CHECK(inventory.inline_bytes <= 65536);
}

// ---------------------------------------------------------------------------
// E12 (and E11's queue witness): the matcher's paged rows, provenance, skips
// and handle scratch against the full rescan it replaces. set_match_row_reuse
// off is the reference computation: it keeps no candidate rows at all and
// rescans the book for every winner, so a difference in any value the run
// produced would mean the paged rows changed the match sequence.

class ReuseHost final : public k3_book::BookHost {
public:
    ReuseHost(const k3_book::BookConfig& config, bool reuse)
        : k3_book::BookHost(config) {
        as_native_consumer(execution_consumer()).set_match_row_reuse(reuse);
    }
};

void matcher_rows(int width) {
    k3_book::BookConfig config;
    config.seed = 907;
    config.live = width;
    config.bars = 32;
    config.path = k3_book::Path::Synthesized;
    config.calc_on_fills = true;
    ReuseHost reused(config, true);
    const auto with = k3_book::run_book(reused, config);
    ReuseHost rescanned(config, false);
    const auto without = k3_book::run_book(rescanned, config);
    CHECK(with.completed && without.completed);
    CHECK(with.trace == without.trace);
    CHECK(with.continuation == without.continuation);
    CHECK(with.broker == without.broker);
    CHECK(with.trades_digest == without.trades_digest);
    CHECK(with.trades == without.trades);
    CHECK(with.events == without.events);
    CHECK(with.events_digest == without.events_digest);
    CHECK(with.position == without.position);
    CHECK(with.accepted == without.accepted);
    CHECK(with.rejected == without.rejected);
    CHECK(with.replaced == without.replaced);
    CHECK(with.cancelled == without.cancelled);
    CHECK(with.applied == without.applied);
    CHECK(with.unordered == 0);

    const auto& consumer = NativeExecutionConsumer::bound(reused);
    // Every applied fill was delivered exactly once (E11's queue): the drain
    // delivered what the kernel committed, whatever its length.
    CHECK(static_cast<std::uint64_t>(with.applied) == consumer.applied_event_count());
    const auto inventory = consumer.resident_inventory();
    CHECK(inventory.driver_rows > 64);
    CHECK(inventory.spilled_rows > 0);
    CHECK(inventory.inline_bytes <= 65536);
    CHECK(inventory.queued_notifications == 0);
}

// ---------------------------------------------------------------------------
// E11 births: a PreOpen callback that submits far more Market births than the
// birth lists' staging holds. The paged list must hold every birth, in the
// order the callback recorded them, while the consumer's resident scratch
// stays at its fixed tail.

class BirthHost final : public NativeStrategyHost {
public:
    static constexpr int kBirths = 100;

    int rejected = 0;
    std::size_t births_held = 0;
    std::size_t grants_held = 0;
    std::uint64_t spilled_at_open = 0;
    std::uint64_t inline_bytes_at_open = 0;
    bool issued = false;
    std::size_t applied = 0;

    void on_native_bar_open(const Bar&, const NativeDecisionContext& context) override {
        if (issued || context.coordinate.path_phase != NativePathPhase::Open) return;
        issued = true;
        auto& consumer = as_native_consumer(execution_consumer());
        // The open activation scope records the same births in its own grant
        // list, so both point-local lists cross the staging bound here.
        if (!consumer.begin_open_activation(*this)) return;
        for (int index = 0; index < kBirths; ++index) {
            no::Request request{no::Transact{1.0}, "birth", ""};
            request.trigger = no::Market{};
            const auto result = submit(request);
            if (!result.handle) ++rejected;
        }
        consumer.end_open_activation();
        // Read inside the callback: the births are durable until the Open's
        // match, and this is the only moment a witness can see them all.
        const auto inventory = consumer.resident_inventory();
        births_held = static_cast<std::size_t>(inventory.pre_open_births);
        grants_held = static_cast<std::size_t>(inventory.activation_births);
        spilled_at_open = inventory.spilled_rows;
        inline_bytes_at_open = inventory.inline_bytes;
    }

    // This fixture issues at PreOpen; bar-close calculations issue no commands.
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        ++applied;
    }
};

void birth_lists() {
    BirthHost host;
    const auto bars = tape(4);
    CHECK(host.configure_native(spec(1)).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.issued);
    CHECK(host.rejected == 0);
    CHECK(host.births_held == static_cast<std::size_t>(BirthHost::kBirths));
    CHECK(host.grants_held == static_cast<std::size_t>(BirthHost::kBirths));
    CHECK(host.spilled_at_open > 0);
    CHECK(host.inline_bytes_at_open <= 65536);
    // Every birth is a Market request born at that Open, so all of them are
    // eligible there: the run applied one fill per birth (the host's own
    // count, not the kernel's).
    CHECK(host.applied == static_cast<std::size_t>(BirthHost::kBirths));
}

// ---------------------------------------------------------------------------
// E06: the cohort-receipt window past its staging. The host replays a known
// command script and records the receipt it expects for each command; the
// window's readback must answer exactly those rows, in order, while the
// resident staging stays at its bound. A Window-retention twin of the same
// script must produce the same continuation and broker hashes: the spill and
// the retirement are storage, not state.

struct ExpectedReceipt {
    no::CohortReceiptOperation operation;
    no::CohortReceiptStatus status;
};

class ReceiptHost final : public NativeStrategyHost {
public:
    static constexpr int kCommandsPerBar = 20;

    explicit ReceiptHost(NativeEventRetention retention) {
        // Both twins read back under a named retention; the spec and every
        // decision are the same, only what a read returns differs.
        as_native_consumer(execution_consumer()).set_retention_override(retention);
    }

    std::vector<ExpectedReceipt> expected;
    int rejected = 0;

    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (!cohort_) cohort_ = cohort_open();
        if (!cohort_) return;
        no::Request resting{no::Transact{1.0}, "resting", ""};
        resting.trigger = no::Limit{50.0};
        const auto accepted = submit(resting);
        if (!accepted.handle) {
            ++rejected;
            return;
        }
        for (int index = 0; index < kCommandsPerBar; ++index) {
            cohort_add(*cohort_, *accepted.handle);
            expected.push_back({no::CohortReceiptOperation::Add,
                                no::CohortReceiptStatus::Applied});
            cohort_remove(*cohort_, *accepted.handle);
            expected.push_back({no::CohortReceiptOperation::Remove,
                                no::CohortReceiptStatus::Applied});
        }
    }

private:
    std::optional<no::CohortHandle> cohort_;
};

struct ReceiptRun {
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
    int trades = 0;
    std::size_t committed = 0;
    std::size_t resident = 0;
    std::size_t window_rows = 0;
    bool rows_match = true;
    std::size_t base = 0;
};

ReceiptRun receipt_run(NativeEventRetention retention, const std::vector<Bar>& bars,
                       std::uint64_t run_number) {
    ReceiptHost host(retention);
    CHECK(host.configure_native(spec(run_number)).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.rejected == 0);
    ReceiptRun out;
    out.continuation = host.native_continuation_hash();
    out.broker = host.broker_state_hash();
    out.trades = host.trade_count();
    const auto& core = host.core();
    out.committed = core.cohort_receipt_count();
    out.base = core.cohort_receipt_base();
    // The resident part: positions the core answers without the store.
    for (std::size_t position = 0; position < out.committed - out.base; ++position) {
        no::CohortReceipt row;
        if (core.cohort_receipt_resident(position, row)) ++out.resident;
    }
    const auto window = core.cohort_receipts();
    out.window_rows = window.size();
    if (retention == NativeEventRetention::Window) {
        out.rows_match = window.empty();
        return out;
    }
    out.rows_match = window.size() == host.expected.size();
    for (std::size_t index = 0; index < host.expected.size() && out.rows_match; ++index) {
        const auto& row = window[index];
        out.rows_match = row.operation == host.expected[index].operation
            && row.status == host.expected[index].status;
    }
    return out;
}

void cohort_receipt_window(int bars) {
    const auto rows = tape(bars);
    // Fresh hosts, one generation: the twins describe the same run, so the
    // only difference between them is what their readback retains.
    const auto full = receipt_run(NativeEventRetention::Full, rows, 1);
    // The population is past the core's staging, so the readback above is a
    // readback of spilled rows, not of a resident window.
    CHECK(full.committed > no::WorkingRequestCore::cohort_receipt_staging());
    CHECK(full.resident <= no::WorkingRequestCore::cohort_receipt_staging());
    CHECK(full.resident < full.committed);
    CHECK(full.window_rows == full.committed - full.base);
    CHECK(full.rows_match);

    // The same script under Window retention, which retires each row after it
    // is folded: the storage differs at every point of the run and every value
    // the run produced must be the same.
    const auto window = receipt_run(NativeEventRetention::Window, rows, 1);
    CHECK(window.committed == full.committed);
    CHECK(window.rows_match);
    CHECK(window.continuation == full.continuation);
    CHECK(window.broker == full.broker);
    CHECK(window.trades == full.trades);

    // A second Full run of the same script reproduces the window exactly.
    const auto again = receipt_run(NativeEventRetention::Full, rows, 1);
    CHECK(again.committed == full.committed);
    CHECK(again.resident == full.resident);
    CHECK(again.rows_match);
}

// The Full batch write buffer must preserve both staged and already spilled
// rows, then return to fixed staging without changing the logical sequence.
void batch_write_buffer_rows() {
    constexpr std::size_t staging = 4;
    pineforge::detail::PageStore store(sizeof(std::uint64_t), staging, "batch buffer witness");
    const auto fixed_bytes = store.resident_bytes();
    std::vector<std::uint64_t> expected;
    std::uint64_t next = 1;
    const auto append = [&](std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t row = next++;
            expected.push_back(row);
            store.push_back(&row);
        }
    };
    const auto check_rows = [&] {
        CHECK(store.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            std::uint64_t row = 0;
            store.read(i, &row);
            CHECK(row == expected[i]);
        }
    };
    append(staging - 1);
    const std::size_t reserved = 3 * staging;
    store.reserve_resident_rows(reserved);
    CHECK(store.resident_capacity() == reserved);
    CHECK(store.resident_bytes() == fixed_bytes + reserved * sizeof(std::uint64_t));
    check_rows();
    append(2 * reserved);
    CHECK(store.spilled_rows() > 0);
    check_rows();
    store.erase_prefix(staging);
    expected.erase(expected.begin(), expected.begin() + staging);
    const std::size_t larger = 2 * reserved;
    store.reserve_resident_rows(larger);
    CHECK(store.resident_capacity() == larger);
    CHECK(store.resident_bytes() == fixed_bytes + larger * sizeof(std::uint64_t));
    check_rows();
    append(larger);
    check_rows();
    store.release_reservation();
    CHECK(store.resident_capacity() == staging);
    CHECK(store.resident_bytes() == fixed_bytes);
    check_rows();
    append(staging + 1);
    store.release_reservation();
    CHECK(store.resident_capacity() == staging);
    CHECK(store.resident_bytes() == fixed_bytes);
    check_rows();
}

}  // namespace

int main() {
    event_readback_rows(16);
    event_readback_rows(128);
    matcher_rows(24);
    matcher_rows(128);
    birth_lists();
    cohort_receipt_window(10);
    cohort_receipt_window(20);
    batch_write_buffer_rows();
    std::printf("test_o1e3_consumer_pages: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
