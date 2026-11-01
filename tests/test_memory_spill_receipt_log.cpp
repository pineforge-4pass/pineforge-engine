// Receipt-log backing growth, resident tails and retirement are checked.
// Complete ReceiptStore/TailLog by including the actual implementation.
// Private access uses the repository's explicit-instantiation
// pattern, including read-cache invalidation before disk verification.
// No injected records, replacement core, or product seam.
// The test reads a retired receipt index to check the rejection path
// (std::out_of_range). WorkingRequestCore::group_effect_receipt asserts
// before it throws, so the included core is built with assertions off.
#ifndef NDEBUG
#define NDEBUG
#endif
#include "../src/native_order.cpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace memory_spill_receipt_log {
namespace no = pineforge::native_order;
using Core = no::WorkingRequestCore;
using Word = std::uint64_t;
using Handle = no::RequestHandle;
using Row = std::tuple<Word, Word, Word, Word>; // cause, recipient, outcome, effect
using Log = no::TailLog<no::ReceiptRec, 64>;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Receipts { friend auto access(Receipts); };
struct File { friend auto access(File); };
struct Backing { friend auto access(Backing); };
struct ReadCache { friend auto access(ReadCache); };
template struct Access<Receipts, &Core::receipt_store_>;
template struct Access<File, &Log::file_>;
template struct Access<Backing, &no::ExtFile::file_>;
template struct Access<ReadCache, &Log::cache_>;

std::size_t checks = 0, failures = 0;
std::string trial;
struct PrerequisiteFailure final : std::exception {};
bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [receipt-log] %s:%d: %s (trial=%s)\n",
                     __FILE__, line, expression, trial.c_str());
    }
    return passed;
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)
// Only an unsafe prerequisite ends its trial; all ordinary checks continue.
#define NEED(expression) do { \
    if (!check(bool(expression), "fixture prerequisite: " #expression, __LINE__)) \
        throw PrerequisiteFailure{}; \
} while (false)

template<class Action>
void run(const std::string& name, Action&& action) {
    trial = name;
    try { action(); }
    catch (const PrerequisiteFailure&) { /* Already reported; next trial. */ }
    catch (const std::exception& error) { check(false, error.what(), __LINE__); }
    catch (...) { check(false, "unknown exception", __LINE__); }
}

// Independently derived from TailWindowConfig::receipt_rows (default 64),
// TailLog::make_room (write the older half BEFORE the next push), and the
// four uint64_t fields of ReceiptRec. Never query the log to derive a bound.
constexpr Word kTail = 64;
constexpr Word kRecordBytes = 4 * sizeof(Word);
Word spilled_rows(Word appended) {
    if (appended <= kTail) return 0;
    constexpr Word half = kTail / 2;
    return ((appended - kTail + half - 1) / half) * half;
}

struct Fixture {
    no::RunIdentity identity{"receipt-log", 1};
    Core core{identity};
    Word incarnation = 1, ordinal = 1, point = 1;
    bool nonflat = false;
    std::vector<Row> model;

    Handle submit(no::Request request) {
        const Handle expected{identity, incarnation};
        const auto result = core.submit(request, 0, incarnation, ordinal);
        NEED(result.status == no::SubmitStatus::Accepted && result.handle);
        CHECK(*result.handle == expected);
        return *result.handle;
    }
    void install(no::Preparation<no::PreparedMutation> preparation) {
        NEED(std::holds_alternative<no::PreparedMutation>(preparation));
        const auto result = core.install_mutation(
            std::get<no::PreparedMutation>(std::move(preparation)));
        NEED(std::holds_alternative<no::Installed>(result));
        ordinal += std::get<no::Installed>(result).events.count;
    }
    no::EventId fill(Handle handle) {
        const no::PositionIdentity before = nonflat
            ? no::PositionIdentity{no::PositionNonflat{1, no::Side::Long}}
            : no::PositionIdentity{no::PositionFlat{}};
        no::EvaluationContext context;
        context.cursor.point.ordinal = point = std::max(point, ordinal) + 1;
        context.cursor.point.effective_time_ms = 1000;
        context.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
        context.driver_class = no::DriverEligibilityClass::ObservedPrint;
        context.existing_matching_bit = true;
        no::TargetObservation observation;
        observation.current_position = before;
        auto evaluated = core.prepare_evaluation(handle, context, observation, ordinal);
        if (std::holds_alternative<no::PreparedMutation>(evaluated))
            install(std::move(evaluated));
        else
            NEED(std::holds_alternative<no::NoChange>(evaluated));
        no::ExecutionProposal proposal;
        proposal.cursor = context.cursor;
        proposal.raw_price = proposal.resolved_price = 100.0;
        proposal.physical_action = no::Transact{1.0};
        proposal.pre_fill = before;
        proposal.pre_target.current_position = before;
        proposal.inspected_opened_units = 1.0;
        proposal.inspected_current_ticket = 1;
        auto prepared = core.prepare_execution(handle, proposal, ordinal);
        NEED(std::holds_alternative<no::PreparedExecution>(prepared));
        // Expected ids come from the command counter before installation,
        // not from the log or the journal that we later check.
        const no::EventId expected{identity, ordinal};
        no::CommittedExecutionFacts facts;
        facts.result.status = pineforge::execution::Status::Applied;
        facts.result.opened_units = 1.0;
        facts.result.current_ticket = 1;
        facts.cycle_before = nonflat ? 1 : 0;
        facts.cycle_after = 1;
        facts.post_target.current_position = no::PositionNonflat{1, no::Side::Long};
        facts.committed_action = no::Transact{1.0};
        const auto result = core.install_execution(
            std::get<no::PreparedExecution>(std::move(prepared)), facts);
        NEED(std::holds_alternative<no::Installed>(result));
        ordinal += std::get<no::Installed>(result).events.count;
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&core.history().back());
        NEED(applied);
        CHECK(applied->ordinal == expected.ordinal && applied->handle() == handle
              && applied->terminal && applied->filled_working == 1.0);
        nonflat = true;
        return expected;
    }
    void append(Handle target, bool direct) {
        const auto before = model.size();
        no::Request source{no::Transact{1.0}, "receipt-source", ""};
        source.group = no::Member{7, 1, no::GroupEffect::Reduce};
        const auto cause = fill(submit(std::move(source)));
        // Submit and fill are controls: neither is a group-effect receipt.
        CHECK(core.group_effect_receipt_count() == before);
        const Word outcome = ordinal;
        if (direct) {
            const auto installed = core.apply_group_effect(cause, target, ordinal);
            NEED(std::holds_alternative<no::Installed>(installed));
            ordinal += std::get<no::Installed>(installed).events.count;
        } else {
            install(core.prepare_group_effect(cause, target, ordinal));
        }
        CHECK(ordinal == outcome + 1);
        model.emplace_back(cause.ordinal, target.incarnation, outcome,
                           static_cast<Word>(no::GroupEffect::Reduce));
        CHECK(core.group_effect_receipt_count() == model.size());
        // Replay traverses the receipt lookup, but must not append a new key.
        if (direct) {
            const auto replay = core.apply_group_effect(cause, target, ordinal);
            const auto* unchanged = std::get_if<no::NoChange>(&replay);
            CHECK(unchanged && unchanged->reason == no::NoChangeReason::AlreadyApplied);
        } else {
            const auto replay = core.prepare_group_effect(cause, target, ordinal);
            const auto* unchanged = std::get_if<no::NoChange>(&replay);
            CHECK(unchanged && unchanged->reason == no::NoChangeReason::AlreadyApplied);
        }
        CHECK(core.group_effect_receipt_count() == model.size() && ordinal == outcome + 1);
    }
};

// epoch_origin counts previously fully retired records. A partial retirement
// changes the valid domain, but does not compact the file or advance origin.
Word observe(Fixture& fixture, Word base, Word epoch_origin) {
    const auto& store = fixture.core.*access(Receipts{});
    NEED(store);
    auto& log = store->log;
    const auto& file = log.*access(File{});
    const auto& backing = file.*access(Backing{});
    const Word count = fixture.model.size();
    const Word written = spilled_rows(count - epoch_origin);
    CHECK(log.size() == count && log.base() == base && log.retained() == count - base);
    CHECK(fixture.core.group_effect_receipt_count() == count
          && fixture.core.group_effect_receipt_base() == base);
    CHECK(log.capacity() == kTail && log.resident() == count - epoch_origin - written
          && log.resident() <= kTail);
    CHECK(file.size() == written * kRecordBytes);
    CHECK(backing.storage_bytes() == written * kRecordBytes);
    CHECK(backing.disk_backed() == (written != 0));
    // Make the read-count proof below a cold-cache disk roundtrip.
    // TailLog::get routes spilled rows through cache_.get;
    // ReadThroughCache returns cached bytes without backing I/O.
    // Earlier observe scans and WorkingRequestCore::retire_history's receipt
    // bisection can warm every retained spilled row. Invalidate only these
    // ReadThroughWindow tags: ReadThroughCache::invalidate_if keeps the
    // allocation and backing intact. Thus base < epoch_origin + written
    // still requires a real file read, with every record checked below.
    (log.*access(ReadCache{})).invalidate();
    const auto reads_before = file.read_count();
    std::vector<Row> actual, public_actual;
    for (Word index = base; index < count; ++index) {
        const auto record = log.get(index);
        actual.emplace_back(record.cause, record.recipient, record.outcome, record.effect);
        const auto receipt = fixture.core.group_effect_receipt(index);
        CHECK(receipt.cause.run == fixture.identity && receipt.recipient.run == fixture.identity);
        public_actual.emplace_back(receipt.cause.ordinal, receipt.recipient.incarnation,
                                   receipt.outcome_ordinal, static_cast<Word>(receipt.effect));
    }
    const std::vector<Row> expected(fixture.model.begin() + static_cast<std::ptrdiff_t>(base),
                                    fixture.model.end());
    CHECK(actual == expected && public_actual == expected);
    if (base < epoch_origin + written) CHECK(file.read_count() > reads_before);
    return file.size();
}

Word receipt_growth(Word population, bool direct) {
    Fixture fixture;
    constexpr Word again = 73; // Cross 64 again after complete retirement.
    no::Request target_request{no::Transact{static_cast<double>(population + again + 10)},
                               "receipt-target", ""};
    target_request.trigger = no::Limit{80.0};
    target_request.group = no::Member{7, 2, no::GroupEffect::Reduce};
    const Handle target = fixture.submit(std::move(target_request));
    for (Word i = 0; i < population; ++i) {
        fixture.append(target, direct);
        if (i + 1 == kTail || i + 1 == kTail + 1) observe(fixture, 0, 0);
    }
    const Word first_bytes = observe(fixture, 0, 0);
    CHECK(first_bytes != 0); // Independent evidence that the spill insert ran.
    const auto* live = fixture.core.find_live(target);
    NEED(live);
    const auto* remaining = std::get_if<no::RemainingUnits>(&live->remaining);
    CHECK(remaining && remaining->q == static_cast<double>(again + 10));
    // No retire call so far: all N receipts survive unrelated submit/fill
    // commands and duplicate drains. This is the core route used by retaining
    // consumers; Full/Commands themselves are not configured by this fixture.
    const Word retired_rows = population / 2;
    const Word through = std::get<0>(fixture.model[retired_rows - 1]);
    CHECK(fixture.core.retire_history(through) > 0);
    CHECK(fixture.core.retired_through() == through);
    CHECK(observe(fixture, retired_rows, 0) == first_bytes);
    // Partial retirement leaves disk extent unchanged; it only moves base.
    bool rejected = false;
    try { (void)fixture.core.group_effect_receipt(retired_rows - 1); }
    catch (const std::out_of_range&) { rejected = true; }
    CHECK(rejected);
    const auto& store = fixture.core.*access(Receipts{});
    NEED(store);
    CHECK(fixture.core.retire_history(fixture.core.last_ordinal()) > 0);
    CHECK(fixture.core.history().empty());
    observe(fixture, population, population);
    // Absolute receipt indices continue across a complete retire; a fresh
    // file begins at this population, rather than leaving a dead prefix.
    for (Word i = 0; i < again; ++i) fixture.append(target, direct);
    CHECK(fixture.model.size() == population + again);
    CHECK(observe(fixture, population, population) == spilled_rows(again) * kRecordBytes);
    const auto cancelled = fixture.core.cancel(target, fixture.ordinal);
    CHECK(cancelled.status == no::CancelStatus::Cancelled);
    CHECK(fixture.core.live_size() == 0);
    observe(fixture, population, population);
    // Reset must release a populated store too, not only an already empty log.
    std::weak_ptr old = store;
    fixture.core.reset({"receipt-log", 2});
    CHECK(old.expired() && !(fixture.core.*access(Receipts{})));
    CHECK(fixture.core.group_effect_receipt_count() == 0
          && fixture.core.group_effect_receipt_base() == 0);
    return first_bytes;
}
} // namespace memory_spill_receipt_log

int main() {
    using namespace memory_spill_receipt_log;
    for (bool direct : {false, true}) {
        Word small_bytes = 0, large_bytes = 0;
        run(direct ? "direct N=97" : "prepared N=97",
            [&] { small_bytes = receipt_growth(97, direct); });
        run(direct ? "direct 4N=388" : "prepared 4N=388",
            [&] { large_bytes = receipt_growth(4 * 97, direct); });
        run(direct ? "direct growth comparison" : "prepared growth comparison", [&] {
            CHECK(small_bytes == spilled_rows(97) * kRecordBytes);
            CHECK(large_bytes == spilled_rows(4 * 97) * kRecordBytes);
            CHECK(large_bytes > small_bytes && small_bytes > 0);
        });
    }
    std::printf("memory proof spill_receipt_log: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
