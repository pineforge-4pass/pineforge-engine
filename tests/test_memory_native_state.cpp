// Native fill-state storage follows the submitted orders and observations.
// The tests use the existing observation interfaces without adding product hooks.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/native_execution_consumer.hpp"
#include <pineforge/detail/scratch_directory.hpp>
#include <pineforge/execution_observer.h>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Consumer = NativeExecutionConsumer;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cache { friend auto access(Cache); };
struct CacheSize { friend auto access(CacheSize); };
struct CachedTarget { friend auto access(CachedTarget); };
struct ReadTarget { friend auto access(ReadTarget); };
struct ReadOpenings { friend auto access(ReadOpenings); };
struct Command { friend auto access(Command); };
struct Candidate { friend auto access(Candidate); };
struct Presented { friend auto access(Presented); };
struct PresentedSize { friend auto access(PresentedSize); };
struct DropPresentation { friend auto access(DropPresentation); };
struct Materialized { friend auto access(Materialized); };
struct MarkCount { friend auto access(MarkCount); };
struct Observe { friend auto access(Observe); };
template struct Access<Cache, &Consumer::cohort_target_cache_>;
template struct Access<CacheSize, &Consumer::cohort_target_cache_size_>;
template struct Access<CachedTarget, &Consumer::cached_cohort_target>;
template struct Access<ReadTarget, &Consumer::read_target_into>;
template struct Access<ReadOpenings, &Consumer::read_openings>;
template struct Access<Command, &Consumer::make_command_context>;
template struct Access<Candidate, &Consumer::inspect_candidate>;
template struct Access<Presented, &Consumer::presentation_open_trades_>;
template struct Access<PresentedSize, &Consumer::presentation_open_trades_len_>;
template struct Access<DropPresentation, &Consumer::drop_presentation>;
template struct Access<Materialized, &OpenReportRowSource::materialized_>;
template struct Access<MarkCount, &OpenReportRowSource::count_>;
template struct Access<Observe, &Consumer::set_execution_observer_v1>;

std::size_t checks = 0, failures = 0;
const char* current_row = "startup";

#define MEM_CHECK(expression) do {                                                \
    ++checks;                                                                   \
    if (!(expression)) {                                                        \
        ++failures;                                                             \
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", current_row,              \
                     __FILE__, __LINE__, #expression);                          \
    }                                                                           \
} while (false)

template<class Action>
void run_row(const char* row, Action&& action) {
    current_row = row;
    try { action(); }
    catch (const std::exception& error) {
        ++checks; ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: exception: %s\n", row,
                     __FILE__, __LINE__, error.what());
    } catch (...) {
        ++checks; ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: unknown exception\n", row,
                     __FILE__, __LINE__);
    }
}

constexpr std::int64_t start = INT64_C(1736121600000);
std::vector<Bar> tape(std::size_t count) {
    std::vector<Bar> bars;
    for (std::size_t i = 0; i < count; ++i)
        bars.push_back({100, 101, 99, 100, 1000,
                        start + static_cast<std::int64_t>(i) * 60000});
    return bars;
}
NativeRunSpec spec(std::uint64_t generation = 1) {
    NativeRunSpec result;
    result.identity = {"native-fill", generation};
    result.input_tf = result.script_tf = "1";
    result.tickerid = "TEST:FILL";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = 1e12;
    result.point_value = result.account_fx = 1;
    result.price_tick = 0.25;
    result.event_retention = NativeEventRetention::Full;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    result.report_open_position_at_end = true;
    return result;
}

struct Host final : NativeStrategyHost {
    std::function<void(Host&)> beginning, calculation;
    std::function<void(Host&, const NativePrecommitView&)> precommit;
    std::size_t bars = 0, fills = 0;
    Consumer& consumer() { return as_native_consumer(execution_consumer()); }
    const OpenReportRowSource& report_rows() const { return range_end_trades_; }
    std::size_t open_lots() const { return pyramid_entries_.size(); }
    void on_native_run_begin() override {
        bars = fills = 0;
        if (beginning) beginning(*this);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars;
        if (calculation) calculation(*this);
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override { ++fills; }
    NativePrecommitVerdict validate_execution_precommit(const NativePrecommitView& view) const override {
        if (precommit) precommit(const_cast<Host&>(*this), view);
        return NativePrecommitVerdict::Admit;
    }
};

void run(Host& host, std::size_t count, std::uint64_t generation = 1,
         const pf_execution_observer_v1* observer = nullptr) {
    const auto setup = host.configure_native(spec(generation));
    MEM_CHECK(setup.status == NativeSetupStatus::Applied);
    if (setup.status != NativeSetupStatus::Applied) throw std::runtime_error("native setup refused");
    if (observer) MEM_CHECK(access(Observe{})(&host, observer) == 0);
    const auto bars = tape(count);
    host.run(bars.data(), static_cast<int>(bars.size()));
    // NativeStrategyHost::run renders begin/capture failures into last_error;
    // show the cause
    // before checking the lifecycle and the downstream memory observations.
    if (!host.last_error().empty())
        std::fprintf(stderr, "native run [%s]: %s\n", current_row, host.last_error().c_str());
    MEM_CHECK(host.last_error().empty());
    MEM_CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    MEM_CHECK(host.last_error().empty() && host.bars == count);
}
no::RequestHandle put(Host& host, const no::Request& request) {
    const auto result = host.submit(request);
    MEM_CHECK(result.status == no::SubmitStatus::Accepted && result.handle.has_value());
    if (result.status != no::SubmitStatus::Accepted || !result.handle)
        throw std::runtime_error("real submit refused: " + request.label);
    return *result.handle;
}
no::ExecutionAppliedEvent apply(Host& host, const no::RequestHandle& handle) {
    const auto result = host.execute_current({handle, NativeCurrentPriceRule::AsPresented});
    MEM_CHECK(std::holds_alternative<no::ExecutionAppliedEvent>(result));
    if (!std::holds_alternative<no::ExecutionAppliedEvent>(result))
        throw std::runtime_error("real current fill refused");
    return std::get<no::ExecutionAppliedEvent>(result);
}
no::LiveRequest live(Host& host, const no::RequestHandle& handle) {
    const auto* found = host.consumer().request_core().find_live(handle);
    MEM_CHECK(found != nullptr);
    if (!found) throw std::runtime_error("real working request unavailable");
    return *found;
}

std::vector<std::uint64_t> ids(const std::vector<no::RequestHandle>& handles) {
    std::vector<std::uint64_t> result;
    for (const auto& handle : handles) result.push_back(handle.incarnation);
    std::sort(result.begin(), result.end());
    return result;
}
bool equal_openings(const std::vector<no::OpeningObservation>& actual,
                    const std::vector<no::RequestHandle>& expected,
                    std::int64_t cycle, bool still_live = true) {
    if (actual.size() != expected.size()) return false;
    const auto wanted = ids(expected);
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].queried_opening.incarnation != wanted[i]
            || actual[i].queried_opening.run != expected.front().run
            || actual[i].queried_cycle != cycle
            || actual[i].has_live_matching_lot != still_live) return false;
    }
    return true;
}

// The cache is an array of 16 entries, guarded by its
// size(). The independent count for q distinct reads is min(q,16); each
// target below binds all K one-unit fills, giving 16*K retained observations.
constexpr std::size_t cache_limit = 16;
void cache_case(std::size_t n) {
    Host host;
    std::size_t observations = 0;
    host.calculation = [&](Host& h) {
        if (h.bars != 1) return;
        auto& consumer = h.consumer();
        const auto& cache = consumer.*access(Cache{});
        MEM_CHECK(cache.size() == cache_limit);
        MEM_CHECK((consumer.*access(CacheSize{})) == 0);
        for (const auto& entry : cache) MEM_CHECK(entry.target.openings.empty());
        if (n == 0) { ++observations; return; }
        const auto cohort = h.cohort_open();
        std::vector<no::RequestHandle> openings;
        std::int64_t cycle = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto opening = put(h, {no::Transact{1}, "cache-lot-" + std::to_string(i), ""});
            h.cohort_add(cohort, opening);
            const auto event = apply(h, opening);
            MEM_CHECK(event.opened_units == 1);
            cycle = event.cycle_after;
            openings.push_back(opening);
        }
        std::vector<no::RequestHandle> closes;
        for (std::size_t i = 0; i < cache_limit + 4; ++i) {
            no::Request request{no::HostSized{no::HostSizedKind::Close, std::nullopt},
                                "cache-close-" + std::to_string(i), ""};
            request.owner = no::BindCohort{cohort};
            request.trigger = no::Limit{1000};
            closes.push_back(put(h, request));
        }
        for (std::size_t i = 0; i < closes.size(); ++i) {
            const auto request = live(h, closes[i]);
            const auto* target = (consumer.*access(CachedTarget{}))(h, request);
            MEM_CHECK((consumer.*access(CacheSize{})) == std::min(i + 1, cache_limit));
            MEM_CHECK((target != nullptr) == (i < cache_limit));
            if (target) MEM_CHECK(equal_openings(target->openings, openings, cycle));
        }
        std::size_t payload = 0;
        for (const auto& entry : cache) {
            MEM_CHECK(entry.target.openings.size() == n
                     && equal_openings(entry.target.openings, openings, cycle));
            payload += entry.target.openings.size();
        }
        MEM_CHECK(payload == cache_limit * n);
        // A hit cannot add an entry or duplicate the observation population.
        const auto again = live(h, closes.front());
        const auto* hit = (consumer.*access(CachedTarget{}))(h, again);
        MEM_CHECK(hit != nullptr && (consumer.*access(CacheSize{})) == cache_limit);
        MEM_CHECK(h.cancel(closes.front()).status == no::CancelStatus::Cancelled);
        MEM_CHECK((consumer.*access(CacheSize{})) == 0);
        payload = 0;
        for (const auto& entry : cache) payload += entry.target.openings.size();
        MEM_CHECK(payload == cache_limit * n); // Reset keeps the old payloads.
        const auto closed = apply(h, put(h, {no::Flatten{}, "cache-flat", ""}));
        MEM_CHECK(closed.closed_units == static_cast<double>(n) && h.open_lots() == 0);
        const auto after = live(h, closes[1]);
        const auto* flat = (consumer.*access(CachedTarget{}))(h, after);
        MEM_CHECK(flat != nullptr && flat->openings.empty());
        payload = 0;
        for (const auto& entry : cache) payload += entry.target.openings.size();
        // Overwriting slot 0 releases its K observations; the other fifteen
        // slots deliberately keep theirs. This is not a zero-retention claim.
        MEM_CHECK((consumer.*access(CacheSize{})) == 1 && payload == (cache_limit - 1) * n);
        ++observations;
    };
    run(host, 4);
    MEM_CHECK(observations == 1 && host.fills == (n == 0 ? 0 : n + 1));
}

void selection_case(std::size_t n) {
    Host host;
    std::size_t observed_precommit = 0, observed_journal = 0, observed = 0;
    host.calculation = [&](Host& h) {
        if (h.bars != 1) return;
        std::vector<no::RequestHandle> openings, selected;
        std::int64_t cycle = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto handle = put(h, {no::Transact{2}, "select-lot-" + std::to_string(i), ""});
            const auto event = apply(h, handle);
            MEM_CHECK(event.opened_units == 2);
            cycle = event.cycle_after;
            openings.push_back(handle);
            if (i % 2 == 0) selected.push_back(handle);
        }
        const std::size_t m = n / 2; // Exactly the even-numbered lots.
        MEM_CHECK(selected.size() == m);
        no::Request request{no::Reduce{no::ExplicitUnits{static_cast<double>(2 * m)}},
                            "selected-close", ""};
        request.owner = no::BindOpenings{selected, cycle, {}};
        auto& consumer = h.consumer();
        const auto context = (consumer.*access(Command{}))(h, request, no::CommandSurface::General);
        MEM_CHECK(context.openings.size() == m && equal_openings(context.openings, selected, cycle));
        const auto closing = put(h, request);
        const auto definition = live(h, closing);
        no::TargetObservation target;
        std::vector<no::RequestHandle> handles;
        (consumer.*access(ReadTarget{}))(h, &definition, target, handles);
        MEM_CHECK(target.openings.size() == m && equal_openings(target.openings, selected, cycle));
        // Reuse the SAME actual TargetObservation, then take the no-target
        // boundary: the previous command's payload must be replaced by empty.
        (consumer.*access(ReadTarget{}))(h, nullptr, target, handles);
        MEM_CHECK(target.openings.empty());
        h.precommit = [&, closing](Host& current, const NativePrecommitView& view) {
            if (view.target != closing) return;
            const auto* scope = std::get_if<no::SelectedExposure>(&view.scope);
            MEM_CHECK(scope != nullptr);
            if (scope) MEM_CHECK(scope->incarnations.size() == m && scope->incarnations == ids(selected));
            const auto working = live(current, closing);
            const auto candidate = (current.consumer().*access(Candidate{}))(
                current, working, view.cursor, view.resolved_price, 1, &view.plan);
            MEM_CHECK(candidate.selected.has_value());
            if (candidate.selected)
                MEM_CHECK(candidate.selected->incarnations.size() == m
                         && candidate.selected->incarnations == ids(selected));
            const auto* candidate_scope = std::get_if<no::SelectedExposure>(&candidate.scope);
            MEM_CHECK(candidate_scope != nullptr);
            if (candidate_scope)
                MEM_CHECK(candidate_scope->incarnations.size() == m
                         && candidate_scope->incarnations == ids(selected));
            MEM_CHECK(candidate.target.openings.size() == m
                     && equal_openings(candidate.target.openings, selected, cycle));
            ++observed_precommit;
        };
        const auto event = apply(h, closing);
        h.precommit = {};
        MEM_CHECK(event.closed_units == static_cast<double>(2 * m) && h.open_lots() == n - m);
        const auto* event_scope = std::get_if<no::SelectedExposure>(&event.scope);
        MEM_CHECK(event_scope != nullptr);
        if (event_scope)
            MEM_CHECK(event_scope->incarnations.size() == m && event_scope->incarnations == ids(selected));
        for (const auto& item : consumer.request_core().history()) {
            const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&item);
            if (!applied || applied->handle() != closing) continue;
            const auto* scope = std::get_if<no::SelectedExposure>(&applied->scope);
            MEM_CHECK(scope != nullptr);
            if (scope) MEM_CHECK(scope->incarnations.size() == m && scope->incarnations == ids(selected));
            ++observed_journal;
        }
        // A disjoint command owns n-m remaining openings, not the previous
        // selection plus this one. Every vector is built by the real reader.
        std::vector<no::RequestHandle> remaining;
        for (std::size_t i = 1; i < openings.size(); i += 2) remaining.push_back(openings[i]);
        no::Request next{no::Reduce{no::ExplicitUnits{static_cast<double>(2 * (n - m))}},
                         "remaining-close", ""};
        next.owner = no::BindOpenings{remaining, cycle, {}};
        const auto next_context = (consumer.*access(Command{}))(h, next, no::CommandSurface::General);
        MEM_CHECK(next_context.openings.size() == n - m
                 && equal_openings(next_context.openings, remaining, cycle));
        const auto final = apply(h, put(h, next));
        MEM_CHECK(final.closed_units == static_cast<double>(2 * (n - m)) && h.open_lots() == 0);
        ++observed;
    };
    run(host, 4);
    MEM_CHECK(observed == 1 && observed_precommit == 1 && observed_journal == 1);
    MEM_CHECK(host.fills == n + 2);
}

void selection_empty_case() {
    Host host;
    std::size_t observed = 0;
    host.calculation = [&](Host& h) {
        if (h.bars != 1) return;
        no::Request request{no::Transact{1}, "unfilled-selection-baseline", ""};
        request.trigger = no::Limit{10};
        auto& consumer = h.consumer();
        const auto context = (consumer.*access(Command{}))(h, request, no::CommandSurface::General);
        const auto handle = put(h, request);
        const auto working = live(h, handle);
        const auto point = h.current_execution_point();
        MEM_CHECK(point.has_value());
        if (!point) return;
        const no::MatchCursor cursor{point->decision.coordinate, 0};
        const auto candidate = (consumer.*access(Candidate{}))(
            h, working, cursor, point->price, 1, nullptr);
        MEM_CHECK(context.openings.empty() && candidate.target.openings.empty()
                 && !candidate.selected.has_value()
                 && std::holds_alternative<execution::Book>(candidate.scope));
        ++observed;
    };
    run(host, 4);
    MEM_CHECK(observed == 1 && host.fills == 0 && host.open_lots() == 0);
}

struct RecordingWindow {
    bool previous = global_allocation::recording;
    RecordingWindow() { global_allocation::recording = true; }
    ~RecordingWindow() { global_allocation::recording = previous; }
};

void returned_openings_case(std::size_t n) {
    Host host;
    std::size_t observed = 0;
    host.calculation = [&](Host& h) {
        if (h.bars != 1) return;
        std::vector<no::RequestHandle> openings;
        std::int64_t cycle = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto handle = put(h, {no::Transact{1}, "returned-" + std::to_string(i), ""});
            const auto event = apply(h, handle);
            MEM_CHECK(event.opened_units == 1);
            openings.push_back(handle);
            cycle = event.cycle_after;
        }
        const auto read_and_release = [&](bool carrying) {
            const auto before = global_allocation::live_bytes;
            RecordingWindow recording;
            {
                // This is the original read_openings::out buffer moved into
                // its owning return value, not a separate fixture container.
                const auto actual = (h.consumer().*access(ReadOpenings{}))(h, openings, cycle);
                MEM_CHECK(actual.size() == n && equal_openings(actual, openings, cycle, carrying));
                // reserve(handles.size()) supplies one scalar-row vector.
                // Allow a factor of two for the library's capacity policy.
                // The short run identity fits the inline string storage on
                // the supported standard libraries; there is no text
                // allocation outside the OpeningObservation row objects.
                MEM_CHECK(actual.capacity() <= 2 * n);
                MEM_CHECK(global_allocation::live_bytes >= before
                         && global_allocation::live_bytes - before
                            <= 2 * n * sizeof(no::OpeningObservation));
            }
            MEM_CHECK(global_allocation::live_bytes == before);
            ++observed;
        };
        read_and_release(n != 0);
        if (n != 0) {
            const auto event = apply(h, put(h, {no::Flatten{}, "returned-flat", ""}));
            MEM_CHECK(event.closed_units == static_cast<double>(n) && h.open_lots() == 0);
            // This reader reports every supplied handle, including a closed
            // one, with its live flag false. Its bound is the input handle
            // count, not a claim that it filters closed handles away.
            read_and_release(false);
        }
    };
    run(host, 4);
    MEM_CHECK(host.fills == (n == 0 ? 0 : n + 1) && observed == (n == 0 ? 1 : 2));
}

// The complete allocation replacement keeps the request size immediately
// before every returned block. TradeC is a trivial C record (no array cookie
// on the supported Itanium-ABI Linux/macOS hosts). This checks the ACTUAL
// owning array's request, rather than trusting only its adjacent length field.
static_assert(std::is_trivially_destructible_v<TradeC>);
std::size_t array_bytes(const TradeC* pointer) {
    if (!pointer) return 0;
    global_allocation::detail::Header header{};
    std::memcpy(&header, reinterpret_cast<const unsigned char*>(pointer) - sizeof(header), sizeof(header));
    return header.size;
}

struct File {
    std::string directory, path;
    File() {
        // report_sink::LineWriter rejects regular files on the borrowed-fd route.
        // Use its path-sink setup instead; both writer and scratch need
        // disk backing.
        std::string refusal;
        int error = 0;
        const auto parent = detail::resolve_scratch_directory({}, refusal, error);
        if (parent.empty())
            throw std::runtime_error("temporary sink directory unavailable: " + refusal);
        std::string pattern = parent + "/pineforge-native-fill-XXXXXX";
        const char* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("temporary sink directory unavailable");
        directory = created;
        path = directory + "/report.ndjson";
    }
    ~File() {
        std::remove(path.c_str());
        ::rmdir(directory.c_str());
    }
};

void presentation_case(std::size_t n, std::size_t bars) {
    File file;
    std::size_t boundaries = 0;
    Host host;
    // An observed generation captures once at terminal_success. Without
    // registration the ordinary report-read path does not own this array.
    const pf_execution_observer_v1 observer{sizeof observer, 1, &boundaries,
        [](void* context, const pf_execution_boundary_v1*, pf_boundary_receipt_v1* receipt) -> int {
            ++*static_cast<std::size_t*>(context);
            receipt->frame_bytes = 0;
            return 0;
        }};
    const pf_report_sink_v1 sink{sizeof sink, 1, file.path.c_str(),
                                 "fill-array", file.directory.c_str()};
    MEM_CHECK(host.set_report_sink_v1(&sink) == 0);
    std::size_t observed = 0;
    host.calculation = [&](Host& h) {
        if (h.bars == 1) {
            for (std::size_t i = 0; i < n; ++i) {
                const auto event = apply(h, put(h, {no::Transact{1}, "present-" + std::to_string(i), ""}));
                MEM_CHECK(event.opened_units == 1);
            }
        } else if (h.bars == 2 && n != 0) {
            const auto event = apply(h, put(h, {
                no::Reduce{no::ExplicitUnits{static_cast<double>(n / 2)}}, "present-half", ""}));
            MEM_CHECK(event.closed_units == static_cast<double>(n / 2));
            ++observed;
        }
    };
    run(host, bars, 1, &observer);
    const std::size_t expected = n - n / 2; // K opens minus K/2 complete FIFO lots.
    auto& consumer = host.consumer();
    const auto& rows = consumer.*access(Presented{});
    const auto count = consumer.*access(PresentedSize{});
    MEM_CHECK(boundaries == 1);
    MEM_CHECK(host.open_lots() == expected && host.fills == n + (n == 0 ? 0 : 1));
    MEM_CHECK(observed == (n == 0 ? 0 : 1));
    MEM_CHECK(count == static_cast<int>(expected) && (rows != nullptr) == (expected != 0));
    MEM_CHECK(array_bytes(rows.get()) == expected * sizeof(TradeC));
    if (rows && count >= 0 && static_cast<std::size_t>(count) == expected) {
        for (std::size_t i = 0; i < expected; ++i) {
            const auto& row = rows[i];
            MEM_CHECK(row.open_at_end == 1 && row.is_long == 1 && row.qty == 1
                     && row.entry_price == 100 && row.exit_price == 100 && row.pnl == 0);
        }
    }
    // Invoke the same retirement used at a new capture, begin and destructor;
    // observation follows the real owning operation, never a test reset.
    (consumer.*access(DropPresentation{}))();
    MEM_CHECK(!(consumer.*access(Presented{})) && (consumer.*access(PresentedSize{})) == 0);
}

void materialized_case(std::size_t n) {
    Host host;
    std::vector<no::RequestHandle> openings;
    host.beginning = [&](Host& h) {
        MEM_CHECK((h.report_rows().*access(Materialized{})).empty());
        MEM_CHECK((h.report_rows().*access(MarkCount{})) == 0);
    };
    host.calculation = [&](Host& h) {
        if (h.bars != 1) return;
        for (std::size_t i = 0; i < n; ++i) {
            const auto handle = put(h, {no::Transact{1}, "materialized-" + std::to_string(i), ""});
            MEM_CHECK(apply(h, handle).opened_units == 1);
            openings.push_back(handle);
        }
    };
    run(host, 8);
    const auto& source = host.report_rows();
    const auto& actual = source.*access(Materialized{});
    MEM_CHECK(host.fills == n && host.open_lots() == n && source.size() == n);
    MEM_CHECK((source.*access(MarkCount{})) == n && actual.empty());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& row = host.materialize_report_trade(static_cast<int>(i));
        MEM_CHECK(actual.size() == i + 1 && actual.size() <= n);
        MEM_CHECK(row.entry_incarnation == openings[i].incarnation && row.qty == 1 && row.open_at_end);
        (void)host.materialize_report_trade(static_cast<int>(i));
        MEM_CHECK(actual.size() == i + 1); // Repeated borrows cannot duplicate rows.
    }
    bool refused = false;
    try { (void)host.materialize_report_trade(static_cast<int>(n)); }
    catch (const std::logic_error&) { refused = true; }
    MEM_CHECK(refused && actual.size() == n && actual.size() <= (source.*access(MarkCount{})));
    host.calculation = {};
    run(host, 32, 2); // A public new run replaces the marked generation.
    MEM_CHECK((host.report_rows().*access(Materialized{})).empty());
    MEM_CHECK((host.report_rows().*access(MarkCount{})) == 0 && host.fills == 0);
}
} // namespace

int main() {
    // Start the isolated return-lifetime windows before other row scenarios
    // call the reader. This also makes a wrongly persistent first allocation
    // visible to the allocation replacement rather than prewarming it.
    for (const std::size_t n : {std::size_t{0}, std::size_t{8}, std::size_t{32}})
        run_row("nativeexecutionconsumer_read_openings_out", [=] { returned_openings_case(n); });
    for (const std::size_t n : {std::size_t{0}, std::size_t{8}, std::size_t{32}})
        run_row("nativeexecutionconsumer_cohort_target_cache", [=] { cache_case(n); });
    run_row("close_selection_worksets", selection_empty_case);
    for (const std::size_t n : {std::size_t{8}, std::size_t{32}})
        run_row("close_selection_worksets", [=] { selection_case(n); });
    for (const std::size_t n : {std::size_t{0}, std::size_t{8}, std::size_t{32}}) {
        for (const std::size_t bars : {std::size_t{8}, std::size_t{32}})
            run_row("nativeexecutionconsumer_presentation_open_trades", [=] { presentation_case(n, bars); });
        run_row("openreportrowsource_materialized", [=] { materialized_case(n); });
    }
    std::printf("memory proof native_state: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
