// memory proof pending_native_scratch: the native consumer's real candidate and fill-drain worksets.
// No implementation .cpp is included, no
// product state is seeded by the test, and no allocation size is measured.
#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Consumer = NativeExecutionConsumer;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct MatchBufferTag { friend auto access(MatchBufferTag); };
struct CandidateRowsTag { friend auto access(CandidateRowsTag); };
struct DrainHandlesTag { friend auto access(DrainHandlesTag); };
struct DrainSeedsTag { friend auto access(DrainSeedsTag); };
template struct Access<MatchBufferTag, &Consumer::match_rows_>;
using MatchBuffer = std::remove_reference_t<decltype(
    std::declval<Consumer&>().*access(MatchBufferTag{}))>;
template struct Access<CandidateRowsTag, &MatchBuffer::rows_>;
template struct Access<DrainHandlesTag, &Consumer::drain_handles_>;
template struct Access<DrainSeedsTag, &Consumer::drain_seeds_>;

std::size_t checks = 0, failures = 0;
std::map<std::string, std::size_t> positive;
const char* dispatched_row = "setup";
std::string trial;
constexpr std::int64_t kStart = 1736121600000LL;

void check(const char* row, bool value, const char* expression,
           const char* reason, int line, std::size_t population = 0) {
    ++checks;
    if (value) {
        if (population != 0) ++positive[row];
        return;
    }
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s (%s; %s)\n",
                 row, __FILE__, line, expression, reason, trial.c_str());
}
#define ROW_CHECK(row, value, reason) check(row, (value), #value, reason, __LINE__)
#define ROW_POP(row, value, reason, population) \
    check(row, (value), #value, reason, __LINE__, (population))
#define FIXTURE(value) ROW_CHECK(dispatched_row, value, "fixture prerequisite")

template<class Function> void attempt(const std::string& label, Function function) {
    trial = label;
    try { function(); }
    catch (const std::exception& error) {
        check(dispatched_row, false, "no exception", error.what(), __LINE__);
    } catch (...) {
        check(dispatched_row, false, "no exception", "non-standard exception", __LINE__);
    }
}

template<class T> std::vector<T> sorted(std::vector<T> values) {
    std::sort(values.begin(), values.end());
    return values;
}

std::vector<Bar> tape(int count, bool release_limits = false) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const double price = release_limits && i >= count / 2 ? 50.0 : 100.0;
        bars.push_back({price, price + 1.0, price - 1.0, price, 1.0, kStart + i * 60000LL});
    }
    return bars;
}

NativeRunSpec spec(const char* id) {
    NativeRunSpec value;
    value.identity = {id, 1};
    value.input_tf = value.script_tf = "1";
    value.tickerid = "TEST:PENDING";
    value.timezone = "UTC";
    value.session = "24x7";
    value.initial_capital = 1.0e9;
    value.price_tick = 0.25;
    value.point_value = value.account_fx = 1.0;
    value.close_execution = NativeCloseExecution::NextEligiblePoint;
    value.event_retention = NativeEventRetention::Full;
    return value;
}

class Host : public NativeStrategyHost {
public:
    Consumer& consumer() { return as_native_consumer(execution_consumer()); }
    const Consumer& consumer() const { return const_cast<Host*>(this)->consumer(); }
    const no::WorkingRequestCore& core() const { return consumer().request_core(); }
    const auto& candidates() const {
        return (consumer().*access(MatchBufferTag{})).*access(CandidateRowsTag{});
    }
    const auto& drain_handles() const { return consumer().*access(DrainHandlesTag{}); }
    const auto& drain_seeds() const { return consumer().*access(DrainSeedsTag{}); }
    no::RequestHandle put(const no::Request& request) {
        const auto result = submit(request);
        FIXTURE(result.status == no::SubmitStatus::Accepted && result.handle.has_value());
        return result.handle.value_or(no::RequestHandle{});
    }
    void withdraw(const no::RequestHandle& handle) {
        FIXTURE(cancel(handle).status == no::CancelStatus::Cancelled);
    }
    void on_native_run_begin() override { bar_ = -1; callbacks = 0; }
    std::size_t callbacks = 0;
protected:
    int bar_ = -1;
};

class MatchHost final : public Host {
public:
    MatchHost(std::size_t population, int horizon)
        : population_(population), horizon_(horizon) {
        // The eager book scan and normal kept-row computation share the real
        // matcher in one non-default mode: dormant classification is
        // disabled (row reuse keeps its default, enabled). All N pending
        // entries participate, so the
        // population at its four-row threshold is script-derived.
        consumer().set_match_dormant(false);
        consumer().set_match_row_reuse(true);
    }
    std::size_t fills = 0, terms = 0, positive_terms = 0;
    bool held = false, closed = false;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        ++callbacks;
        if (bar_ == 0) {
            for (std::size_t i = 0; i < population_; ++i) {
                no::Request request{no::Transact{1.0}, "pending-match-" + std::to_string(i), ""};
                request.trigger = no::Limit{50.0};
                active_.insert(put(request).incarnation);
            }
        }
        if (bar_ > 0 && bar_ < horizon_ / 2) {
            FIXTURE(core().live_size() == population_ && fills == 0);
            held = true;
        }
        if (bar_ == horizon_ / 2) {
            ROW_CHECK("nativeexecutionconsumer_matchrows_rows", fills == population_ && active_.empty()
                        && core().live_size() == 0 && candidates().empty(),
                      "the formerly pending ladder fills and leaves no candidate rows");
        }
        if (bar_ == horizon_ / 2 + 1) put(no::Request{no::Flatten{}, "pending-close", ""});
    }

    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        if (facts.definition && facts.definition->request.label.find("pending-match-") == 0) {
            auto& self = *const_cast<MatchHost*>(this);
            ++self.terms;
            const auto& rows = candidates();
            std::vector<std::uint64_t> actual;
            for (const auto& candidate : rows) actual.push_back(candidate.handle.incarnation);
            const std::size_t remaining = population_ - fills;
            // match_path retains rows from four live requests onwards. A full
            // scan gives one row per unfilled request. At this common t=0
            // cursor a winner refresh replaces the row it just took, rather
            // than adding a second copy (MatchRows::keep). For 1..3 requests
            // the scan keeps no rows. The model starts with N submit results
            // and removes only notified real fills, independently of scratch.
            constexpr std::size_t keep_from = 4;
            const std::vector<std::uint64_t> expected = remaining >= keep_from
                ? std::vector<std::uint64_t>(active_.begin(), active_.end())
                : std::vector<std::uint64_t>{};
            const auto expected_count = remaining >= keep_from ? remaining : 0;
            ROW_POP("nativeexecutionconsumer_matchrows_rows", active_.size() == remaining
                        && rows.size() == expected_count && sorted(actual) == expected,
                    "the populated match_rows_.rows_ equals the independent not-yet-filled handle model",
                    rows.size());
            if (!rows.empty()) ++self.positive_terms;
        }
        return {facts.default_resolved_price, std::nullopt, no::OpeningShape::Transact};
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        if (event.request().label.find("pending-match-") == 0) {
            // match_path releases this scan immediately before delivering
            // the host's on_native_applied notifications.
            // Merely checking the empty final book would miss that boundary.
            ROW_CHECK("nativeexecutionconsumer_matchrows_rows", candidates().empty(),
                      "each fill retires its entire candidate vector before the applied callback");
            FIXTURE(active_.erase(event.handle().incarnation) == 1);
            ++fills;
        }
        if (event.request().label == "pending-close") closed = true;
    }

private:
    std::size_t population_;
    int horizon_;
    std::set<std::uint64_t> active_;
};

class HandlesHost final : public Host {
public:
    explicit HandlesHost(std::size_t population) : population_(population) {}
    std::vector<std::size_t> observed;
    bool closed = false;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        ++callbacks;
        if (bar_ == 0)
            put(no::Request{no::Transact{static_cast<double>(population_ + 8)}, "pending-hold", ""});
        if (bar_ == 1) {
            for (std::size_t i = 0; i < population_; ++i) {
                no::Request request{no::Reduce{no::ExplicitUnits{1.0}},
                                    "pending-bound-close-" + std::to_string(i), ""};
                request.trigger = no::Limit{200.0};
                const auto handle = put(request);
                handles_.push_back(handle);
                expected_.insert(handle.incarnation);
            }
            FIXTURE(core().live_size() == population_);
        }
        if (bar_ == 2) {
            // The engine's next point evaluates these unfilled reduce rows
            // and binds them to its actual book before the pulse fills.
            std::size_t bound = 0;
            for (const auto& row : core().live())
                if (std::holds_alternative<no::BookClose>(row.authority)) ++bound;
            FIXTURE(bound == population_);
            pulse(0);
        }
        if (bar_ == 4) {
            for (std::size_t i = 0; i < population_ / 2; ++i) {
                withdraw(handles_[i]);
                expected_.erase(handles_[i].incarnation);
            }
            pulse(1);
        }
        if (bar_ == 6) {
            for (std::size_t i = population_ / 2; i < handles_.size(); ++i)
                withdraw(handles_[i]);
            expected_.clear();
            pulse(2);
        }
        if (bar_ == 8) put(no::Request{no::Flatten{}, "pending-handles-close", ""});
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        if (event.request().label.find("pending-handles-pulse-") == 0) {
            const auto& rows = drain_handles();
            std::vector<std::uint64_t> actual;
            for (const auto& handle : rows) actual.push_back(handle.incarnation);
            const std::vector<std::uint64_t> expected(expected_.begin(), expected_.end());
            // The group and waiting-child lists are empty for this pulse.
            // The final real out-parameter refill is bound_close_handles:
            // one row for each of the N, then ceil(N/2), then zero pending
            // closes in our command model. All are never-triggered limits.
            ROW_POP("nativeexecutionconsumer_drain_handles", rows.size() == expected.size()
                        && sorted(actual) == expected,
                    "drain_handles_ is exactly the current bound-close set, with no prior fill's handles",
                    rows.size());
            observed.push_back(rows.size());
        }
        if (event.request().label == "pending-handles-close") closed = true;
    }
private:
    std::size_t population_;
    std::vector<no::RequestHandle> handles_;
    std::set<std::uint64_t> expected_;
    void pulse(int index) {
        put(no::Request{no::Transact{1.0}, "pending-handles-pulse-" + std::to_string(index), ""});
    }
};

class SeedsHost final : public Host {
public:
    explicit SeedsHost(std::size_t population) : population_(population) {}
    std::vector<std::size_t> observed;
    bool closed = false;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        ++callbacks;
        if (bar_ == 0 || bar_ == 4) wave(bar_ / 4);
        if (bar_ == 1 || bar_ == 5)
            put(no::Request{no::Transact{1.0}, "pending-seed-clear-" + std::to_string(bar_), ""});
        if (bar_ == 8) put(no::Request{no::Flatten{}, "pending-seeds-close", ""});
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        const auto& seeds = drain_seeds();
        if (event.request().label.find("pending-seed-fire-") == 0) {
            std::vector<std::uint64_t> actual;
            bool causes_name_this_run = true;
            for (const auto& seed : seeds) {
                actual.push_back(seed.second.incarnation);
                causes_name_this_run = causes_name_this_run && seed.first.run == event.handle().run
                    && seed.second.run == event.handle().run && seed.first.ordinal != 0;
            }
            // N other cohorts in this cancel group become absent. Each adds
            // one seed, then the filler contributes one more: N+1, not the
            // number of all historic OCA commands. The following unrelated
            // fill has no absent parents or waiting children and leaves zero.
            ROW_POP("nativeexecutionconsumer_drain_seeds", seeds.size() == population_ + 1
                        && sorted(actual) == sorted(expected_) && causes_name_this_run,
                    "drain_seeds_ equals N cancelled pending siblings plus their actual filler",
                    seeds.size());
            FIXTURE(core().live_size() == 0);
            observed.push_back(seeds.size());
        }
        if (event.request().label.find("pending-seed-clear-") == 0) {
            ROW_CHECK("nativeexecutionconsumer_drain_seeds", seeds.empty(),
                      "the next unrelated fill cuts the dependency seed group back to empty");
            observed.push_back(seeds.size());
        }
        if (event.request().label == "pending-seeds-close") closed = true;
    }
private:
    std::size_t population_;
    std::vector<std::uint64_t> expected_;
    void wave(int index) {
        expected_.clear();
        const auto group = static_cast<std::uint64_t>(index + 1);
        for (std::size_t i = 0; i < population_; ++i) {
            no::Request request{no::Transact{1.0},
                                "pending-seed-pending-" + std::to_string(index) + "-" + std::to_string(i), ""};
            request.trigger = no::Limit{50.0};
            request.group = no::Member{group, static_cast<std::int64_t>(i + 2), no::GroupEffect::Cancel};
            expected_.push_back(put(request).incarnation);
        }
        no::Request filler{no::Transact{1.0}, "pending-seed-fire-" + std::to_string(index), ""};
        filler.group = no::Member{group, 1, no::GroupEffect::Cancel};
        expected_.push_back(put(filler).incarnation);
        FIXTURE(core().live_size() == population_ + 1);
        FIXTURE(drain_seeds().empty());
    }
};

void match_row() {
    for (const std::size_t population : {17U, 68U}) {
        std::vector<std::size_t> counts;
        for (const int bars : {16, 64}) {
            attempt("match Q=" + std::to_string(population) + " bars=" + std::to_string(bars), [&] {
                MatchHost host(population, bars);
                FIXTURE(host.configure_native(spec("pending-match")).status == NativeSetupStatus::Applied);
                const auto input = tape(bars, true);
                host.run(input.data(), bars);
                FIXTURE(host.last_error().empty() && host.callbacks == static_cast<std::size_t>(bars));
                FIXTURE(host.held && host.closed && host.fills == population && host.terms == population);
                ROW_CHECK("nativeexecutionconsumer_matchrows_rows", host.positive_terms == population - 3,
                          "every fill with at least four not-yet-filled requests inspected a populated vector");
                counts.push_back(host.positive_terms);
            });
        }
        ROW_CHECK("nativeexecutionconsumer_matchrows_rows", counts.size() == 2 && counts[0] == counts[1],
                  "N and 4N input bars produce identical pending candidate populations");
    }
}

void handles_row() {
    for (const std::size_t population : {17U, 68U}) {
        for (const int bars : {16, 64}) {
            attempt("handles Q=" + std::to_string(population) + " bars=" + std::to_string(bars), [&] {
                HandlesHost host(population);
                FIXTURE(host.configure_native(spec("pending-handles")).status == NativeSetupStatus::Applied);
                const auto input = tape(bars);
                host.run(input.data(), bars);
                FIXTURE(host.last_error().empty() && host.callbacks == static_cast<std::size_t>(bars));
                const std::vector<std::size_t> expected{population, population - population / 2, 0};
                ROW_CHECK("nativeexecutionconsumer_drain_handles", host.observed == expected && host.closed,
                          "all three real fill-drain populations reached their independent counts");
            });
        }
    }
}

void seeds_row() {
    for (const std::size_t population : {17U, 68U}) {
        for (const int bars : {16, 64}) {
            attempt("seeds Q=" + std::to_string(population) + " bars=" + std::to_string(bars), [&] {
                SeedsHost host(population);
                FIXTURE(host.configure_native(spec("pending-seeds")).status == NativeSetupStatus::Applied);
                const auto input = tape(bars);
                host.run(input.data(), bars);
                FIXTURE(host.last_error().empty() && host.callbacks == static_cast<std::size_t>(bars));
                const std::vector<std::size_t> expected{population + 1, 0, population + 1, 0};
                ROW_CHECK("nativeexecutionconsumer_drain_seeds", host.observed == expected && host.closed,
                          "each OCA wave and following unrelated fill has the same bounded seed population");
            });
        }
    }
}
struct Row { const char* id; void (*run)(); };
} // namespace

int main() {
    const Row rows[] = {
        {"nativeexecutionconsumer_matchrows_rows", match_row},
        {"nativeexecutionconsumer_drain_handles", handles_row},
        {"nativeexecutionconsumer_drain_seeds", seeds_row},
    };
    for (const auto& row : rows) {
        dispatched_row = row.id;
        attempt(std::string("row dispatch ") + row.id, row.run);
        ROW_CHECK(row.id, positive[row.id] != 0,
                  "a successful positive real-container assertion must have been reached");
    }
    std::printf("memory proof pending_native_scratch: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
