// Exercise unreachable resting orders through the existing core and host APIs.
// This test needs no new engine interface.
//
// The ordinary row tests the narrower, independently sound bulk-refresh gate:
// an entry-only spilled book cannot contain a BookClose to refresh. The
// allocation counter observes the real blob decodes, and value/receipt/token
// checks forbid replacing the operation with an unconditional no-op.
//
// --matcher separately checks the flat book of orders that never fill.
// Skipping allowance refresh alone does not establish the matching bound.
// --property prints deterministic state and receipts from separate builds
// with the spilled-close authority gate enabled and disabled.
// Comparing those observations checks that the gate preserves behavior;
// no public run switch or expected hash derived from product code is used.
// Sizes and behavior expectations are layout estimates from source,
// not measured; execution results must come from the test itself.
#include "global_allocation_replacement.hpp"
#include "native_order_full_fold.hpp"
#include "native_match_book_fixture.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
namespace ff = full_fold;

int failures = 0;
#define CHECK(condition) do {                                                    \
    if (!(condition)) {                                                         \
        ++failures;                                                             \
        std::fprintf(stderr, "FAIL test_native_unreachable_book %s:%d: %s\n", __FILE__, __LINE__, #condition);\
    }                                                                           \
} while (false)
#define REQUIRE(condition) do {                                                  \
    if (!(condition)) throw std::runtime_error(#condition);                      \
} while (false)

// Fixture sizes, not measured costs. The production bound is unchanged.
constexpr std::size_t kResident = 4096;
constexpr std::size_t kPopulation = 4300;

no::Request far_limit(std::size_t index) {
    const bool buy = index % 2 == 0;
    no::Request request{no::Transact{buy ? 1.0 : -1.0},
                        "c3k-" + std::to_string(index), ""};
    request.trigger = no::Limit{buy ? 1.0 : 10000.0};
    return request;
}

no::EvaluationContext print_at(std::uint64_t point) {
    no::EvaluationContext context;
    context.cursor.point.ordinal = point;
    context.cursor.point.effective_time_ms = 1000;
    context.cursor.point.provenance = NativePriceProvenance::ObservedPrint;
    context.driver_class = no::DriverEligibilityClass::ObservedPrint;
    context.existing_matching_bit = true;
    return context;
}

struct Core {
    no::WorkingRequestCore core{no::RunIdentity{"c3k-core", 1}};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    std::vector<no::RequestHandle> handles;

    no::RequestHandle add(const no::Request& request) {
        const auto result = core.submit(request, 0, incarnation, ordinal);
        REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle);
        handles.push_back(*result.handle);
        return *result.handle;
    }

    void bind(const no::RequestHandle& handle, const no::PositionIdentity& position) {
        no::TargetObservation observed;
        observed.current_position = position;
        auto prepared = core.prepare_evaluation(handle, print_at(ordinal + 1),
                                                 observed, ordinal);
        REQUIRE(std::holds_alternative<no::PreparedMutation>(prepared));
        auto installed = core.install_mutation(
            std::get<no::PreparedMutation>(std::move(prepared)));
        REQUIRE(std::holds_alternative<no::Installed>(installed));
        ordinal += std::get<no::Installed>(installed).events.count;
    }
};

void empty_authority_refresh_does_no_decode() {
    Core run;
    for (std::size_t index = 0; index != kPopulation; ++index)
        run.add(far_limit(index));
    REQUIRE(run.core.book_spilled());
    REQUIRE(run.core.live().size() == kPopulation);
    // An abandoned preparation may change the epoch, so take the baseline
    // after preparing. A bulk refresh itself must leave this token valid.
    auto pending = run.core.prepare_cancel(run.handles.front(), run.ordinal);
    const std::uint64_t before = ff::core_value(run.core);
    const std::uint64_t first = run.ordinal + 10;
    const no::PositionIdentity long7 = no::PositionNonflat{7, no::Side::Long};
    const std::size_t allocations_before = global_allocation::allocations;
    for (std::uint64_t point = first; point != first + 3; ++point)
        run.core.refresh_point_allowances(point, long7);
    const std::size_t decodes = global_allocation::allocations - allocations_before;
    if (decodes != 0)
        std::fprintf(stderr, "unreachable book allowance refresh: entry-only book allocated %zu times; expected 0\n",
                     decodes);
    CHECK(decodes == 0);
    CHECK(ff::core_value(run.core) == before);
    for (const auto& row : run.core.live()) {
        CHECK(std::holds_alternative<no::BookTransaction>(row.authority));
        CHECK(std::holds_alternative<no::AllowanceUnset>(row.allowance));
    }
    auto installed = run.core.install_cancel(std::move(pending));
    REQUIRE(std::holds_alternative<no::CommandInstalled<no::CancelResult>>(installed));
    const auto& cancelled = std::get<no::CommandInstalled<no::CancelResult>>(installed);
    CHECK(cancelled.result.status == no::CancelStatus::Cancelled);
    CHECK(cancelled.events.count == 1);
    CHECK(run.core.live().size() == kPopulation - 1);
    CHECK(std::holds_alternative<no::CancelledEvent>(run.core.history().back()));
}

// Independent oracle: a refresh changes only a matching bound BookClose's
// allowance. Every close here has 1 unit remaining and a 1/4-unit point
// budget, so min(1, 1/4) = 1/4 for both initial and left. No production
// evaluated_allowance helper computes the expected value.
void refresh_and_compare(Core& run, std::uint64_t point,
                         const no::PositionIdentity& position) {
    std::vector<no::LiveRequest> expected = run.core.live();
    const auto* nonflat = std::get_if<no::PositionNonflat>(&position);
    if (nonflat) {
        for (auto& row : expected) {
            const auto* close = std::get_if<no::BookClose>(&row.authority);
            if (close && close->cycle == nonflat->cycle && close->side == nonflat->side)
                row.allowance = no::AllowanceUnits{point, 0.25, 0.25};
        }
    }
    const std::size_t history = run.core.history_end();
    const std::uint64_t ordinal = run.core.last_ordinal();
    run.core.refresh_point_allowances(point, position);
    CHECK(run.core.history_end() == history);
    CHECK(run.core.last_ordinal() == ordinal);
    std::size_t index = 0;
    for (const auto& row : run.core.live()) {
        REQUIRE(index < expected.size());
        CHECK(ff::value(row) == ff::value(expected[index]));
        ++index;
    }
    CHECK(index == expected.size());
}

void real_closes_are_not_skipped() {
    Core run;
    const no::PositionIdentity long7 = no::PositionNonflat{7, no::Side::Long};
    const no::PositionIdentity long8 = no::PositionNonflat{8, no::Side::Long};
    // A close in each tier, with entries between them. Binding the last row
    // then adding a row spills an actual BookClose, not an unbound request.
    no::Request close{no::Reduce{no::ExplicitUnits{1.0}}, "close", ""};
    close.capacity = no::PointBudget{0.25};
    close.trigger = no::Limit{10000.0};
    const auto resident = run.add(close);
    run.bind(resident, long7);
    while (run.core.live().size() < kPopulation) run.add(far_limit(run.handles.size()));
    const auto spilled7 = run.add(close);
    run.bind(spilled7, long7);
    run.add(far_limit(run.handles.size()));
    const auto spilled8 = run.add(close);
    run.bind(spilled8, long8);
    run.add(far_limit(run.handles.size()));
    REQUIRE(run.core.book_spilled());
    const std::uint64_t first = run.ordinal + 10;
    refresh_and_compare(run, first, long7);
    refresh_and_compare(run, first, long7); // same-point retry is idempotent
    refresh_and_compare(run, first + 1, no::PositionFlat{});
    refresh_and_compare(run, first + 2, long8);
    refresh_and_compare(run, first + 3, no::PositionNonflat{7, no::Side::Short});
    refresh_and_compare(run, first + 4, long7);
    // Promotion, retirement and a subsequent spill cannot lose the positive
    // authority witness. The entry-only zero case above must still stay fast.
    const auto result = run.core.cancel(spilled7, run.ordinal);
    CHECK(result.status == no::CancelStatus::Cancelled);
    run.add(far_limit(run.handles.size()));
    refresh_and_compare(run, first + 5, long8);
}

NativeRunSpec spec() {
    NativeRunSpec value;
    value.identity = {"c3k-public", 1};
    value.input_tf = "5";
    value.script_tf = "5";
    value.tickerid = "TEST:C3K";
    value.timezone = "UTC";
    value.session = "24x7";
    value.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    value.initial_capital = 1000000.0;
    value.point_value = 1.0;
    value.account_fx = 1.0;
    value.price_tick = 0.25;
    value.event_retention = NativeEventRetention::Full;
    return value;
}

std::vector<Bar> tape() {
    std::vector<Bar> result(3);
    for (std::size_t index = 0; index != result.size(); ++index) {
        auto& bar = result[index];
        bar.open = 100.0;
        bar.high = 101.0;
        bar.low = 99.0;
        bar.close = 100.0;
        bar.volume = 1.0;
        bar.timestamp = 1736121600000LL + static_cast<std::int64_t>(index) * 300000;
    }
    return result;
}

class QuietBook final : public NativeStrategyHost {
public:
    explicit QuietBook(std::size_t population) : population_(population) {}
    no::WorkingRequestCore::BookLookupReads placed{}, matched{};
    std::size_t applied = 0;
    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bars_ == 1) {
            for (std::size_t index = 0; index != population_; ++index) {
                const auto result = submit(far_limit(index));
                REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle);
            }
            placed = core().book_lookup_reads();
        } else {
            matched = core().book_lookup_reads();
        }
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override { ++applied; }
private:
    std::size_t population_;
    int bars_ = 0;
};

void public_no_fill(bool require_matcher_gate) {
    const std::size_t population = require_matcher_gate ? kPopulation : 12;
    QuietBook host(population);
    REQUIRE(host.configure_native(spec()).status == NativeSetupStatus::Applied);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.last_error().empty());
    // Hand geometry: buys at 1 are below low=99, sells at 10000 above
    // high=101 on every bar. There is no fill, exposure, P&L or terminal row.
    CHECK(host.applied == 0);
    CHECK(host.trade_count() == 0);
    CHECK(host.physical_position().signed_units == 0.0);
    CHECK(host.core().live().size() == population);
    std::uint64_t last_match = 0;
    for (const auto& event : host.native_events(0)) {
        // Modeled segments have excursion=true and matching=false, but
        // priced limits are evaluated along them as well as at the open.
        if (event.driver && (event.driver->matching || event.driver->excursion))
            last_match = std::max(last_match, event.driver->coordinate.ordinal);
        if (event.command) CHECK(std::holds_alternative<no::AcceptedEvent>(*event.command));
    }
    REQUIRE(last_match != 0);
    CHECK(host.core().history_end() == population);
    std::size_t index = 0;
    for (const auto& row : host.core().live()) {
        CHECK(row.request().label == "c3k-" + std::to_string(index));
        const auto* allowance = std::get_if<no::AllowanceUnits>(&row.allowance);
        CHECK(allowance != nullptr);
        if (allowance) {
            CHECK(allowance->point_ordinal == last_match);
            CHECK(allowance->initial == 1.0);
            CHECK(allowance->left == 1.0);
        }
        ++index;
    }
    if (require_matcher_gate) {
        const auto rows = host.matched.rows - host.placed.rows;
        const auto steps = host.matched.steps - host.placed.steps;
        if (rows != 0 || steps != 0)
            std::fprintf(stderr, "unreachable book matching: resting orders needed %llu lookup rows, "
                         "%llu rank steps; expected 0, 0\n",
                         static_cast<unsigned long long>(rows),
                         static_cast<unsigned long long>(steps));
        CHECK(rows == 0);
        CHECK(steps == 0);
    }
}

// Xorshift choices are inputs. Values printed below are ACTUAL public-core
// receipts and state, compared across separately built kernels by the lab.
struct Rng {
    std::uint64_t value;
    std::uint64_t next() {
        value ^= value << 13; value ^= value >> 7; value ^= value << 17;
        return value;
    }
};

void property_transcript(std::uint64_t seed) {
    Core run;
    Rng rng{seed};
    for (std::size_t index = 0; index != kResident - 2; ++index)
        run.add(far_limit(index));
    const no::PositionIdentity long7 = no::PositionNonflat{7, no::Side::Long};
    for (std::size_t step = 0; step != 48; ++step) {
        // Cross the real bound in both directions; do not alter kBookMax.
        const std::size_t target = step % 2 == 0 ? kResident + 12 : kResident - 4;
        while (run.core.live().size() < target) {
            run.add(far_limit(static_cast<std::size_t>(run.incarnation)));
        }
        while (run.core.live().size() > target) {
            const auto rows = std::vector<no::LiveRequest>(run.core.live());
            const auto& row = rows[static_cast<std::size_t>(rng.next() % rows.size())];
            const auto result = run.core.cancel(row.handle(), run.ordinal);
            REQUIRE(result.status == no::CancelStatus::Cancelled);
        }
        const auto rows = std::vector<no::LiveRequest>(run.core.live());
        const auto handle = rows[static_cast<std::size_t>(rng.next() % rows.size())].handle();
        no::ReplaceOptions options;
        options.keep_handle = rng.next() % 2 != 0;
        const auto result = run.core.replace(handle, far_limit(run.incarnation), 0,
                                             run.incarnation, run.ordinal,
                                             std::nullopt, options);
        REQUIRE(result.status == no::ReplaceStatus::Replaced);
        // The first phase has no BookClose at all. Later, positive authorities
        // are repeatedly created, spilled, rewritten, promoted and retired.
        if (step == 8 || step == 24) {
            no::Request close{no::Reduce{no::ExplicitUnits{1.0}}, "property-close", ""};
            close.capacity = no::PointBudget{0.25};
            const auto bound = run.add(close);
            run.bind(bound, long7);
        }
        refresh_and_compare(run, run.ordinal + 10, step % 3 == 0
                            ? no::PositionIdentity{no::PositionFlat{}} : long7);
        std::printf("unreachable book core state %llu %zu %llu\n",
                    static_cast<unsigned long long>(seed), step,
                    static_cast<unsigned long long>(ff::core_value(run.core)));
    }
    // Every command receipt, not just a census or a final hash.
    for (const auto& event : run.core.history())
        std::printf("unreachable book core receipt %llu %llu\n", static_cast<unsigned long long>(seed),
                    static_cast<unsigned long long>(ff::value(event)));
}

// Keep the large population in unreachable limits, and let the established
// random native-host fixture churn a small active tail. Unlike fake committed
// facts supplied by a test, these fills pass through the real public run and
// financial execution boundary. The initial opposite market units guarantee
// that the differential includes applied fills and a closed trade.
class PaddedBook final : public k3_book::BookHost {
public:
    explicit PaddedBook(const k3_book::BookConfig& config) : BookHost(config) {}
    std::vector<std::uint64_t> states;
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        const bool first = !seeded_;
        if (first) {
            seeded_ = true;
            for (std::size_t index = 0; index != kResident - 2; ++index) {
                const auto result = submit(far_limit(index));
                REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle);
                ++outcome.accepted;
            }
        }
        BookHost::on_native_bar(bar, context);
        if (first) {
            // After the random churn snapshot: these requests cannot be
            // selected for random cancellation before their first match.
            for (const double units : {1.0, -1.0}) {
                const auto result = submit(no::Request{no::Transact{units}, "cross", ""});
                REQUIRE(result.status == no::SubmitStatus::Accepted && result.handle);
                ++outcome.accepted;
            }
        }
        observe();
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        BookHost::on_native_applied(event, context);
        observe();
    }
    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }
protected:
    void after_command() override { observe(); }
private:
    void observe() {
        states.push_back(ff::core_value(core()));
        states.push_back(native_continuation_hash());
    }
    bool seeded_ = false;
};

void public_property_transcript(std::uint64_t seed) {
    k3_book::BookConfig config;
    config.seed = seed;
    config.live = static_cast<int>(kResident + 8);
    config.bars = 4;
    PaddedBook host(config);
    const auto outcome = k3_book::run_book(host, config);
    REQUIRE(outcome.completed && outcome.error.empty());
    CHECK(outcome.applied >= 2);
    CHECK(outcome.trades >= 1);
    for (std::size_t index = 0; index != host.states.size(); ++index)
        std::printf("unreachable book public state %llu %zu %llu\n",
                    static_cast<unsigned long long>(seed), index,
                    static_cast<unsigned long long>(host.states[index]));
    for (const auto& event : host.native_events(0)) {
        if (event.command)
            std::printf("unreachable book public receipt %llu %llu\n",
                        static_cast<unsigned long long>(seed),
                        static_cast<unsigned long long>(ff::value(*event.command)));
    }
    std::printf("unreachable book public final %llu %llu %llu %llu %d %ld %ld %ld\n",
                static_cast<unsigned long long>(seed),
                static_cast<unsigned long long>(ff::core_value(host.core())),
                static_cast<unsigned long long>(outcome.continuation),
                static_cast<unsigned long long>(outcome.broker), outcome.trades,
                outcome.applied, outcome.replaced, outcome.cancelled);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--matcher") == 0) {
            public_no_fill(true);
        } else if (argc == 2 && std::strcmp(argv[1], "--property") == 0) {
            property_transcript(0x452391bULL);
            property_transcript(0xc3b4300ULL);
            public_property_transcript(0x452391bULL);
            public_property_transcript(0xc3b4300ULL);
        } else if (argc == 1) {
            empty_authority_refresh_does_no_decode();
            real_closes_are_not_skipped();
            public_no_fill(false);
        } else {
            std::fprintf(stderr, "usage: test_native_unreachable_book [--matcher|--property]\n");
            return 2;
        }
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL test_native_unreachable_book prerequisite/exception: %s\n", error.what());
    }
    return failures == 0 ? 0 : 1;
}
