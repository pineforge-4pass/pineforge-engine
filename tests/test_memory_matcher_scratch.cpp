// memory proof matcher_scratch: actual consumer scratch, not allocation-call counters.
// Expected populations are derived from the submitted requests.
#include <pineforge/native_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Consumer = NativeExecutionConsumer;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEMBER(tag, field) \
    struct tag { friend auto access(tag); }; \
    template struct Access<tag, &Consumer::field>
MEMBER(Trails, match_trail_handles_);
MEMBER(Target, match_target_);
MEMBER(TargetHandles, match_target_handles_);
MEMBER(Requests, requests_);
MEMBER(CollectTrails, collect_match_trails);
MEMBER(CohortSide, scratch_cohort_side);
MEMBER(Pending, pending_bar_views_);
MEMBER(Active, active_bar_views_);
MEMBER(SubBars, intrabar_sub_bars_);
MEMBER(Samples, intrabar_samples_);
#undef MEMBER

std::size_t checks = 0, failures = 0;
#define CHECK(row, expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", row, __FILE__, __LINE__, #expression); \
    } \
} while (false)
template<class F> void scenario(const char* row, F&& f) {
    try { f(); }
    catch (const std::exception& e) {
        CHECK(row, false);
        std::fprintf(stderr, "  exception [%s]: %s\n", row, e.what());
    }
    catch (...) { CHECK(row, false); }
}

constexpr std::int64_t epoch = 1736121600000LL;
constexpr std::int64_t minute = 60000;
Bar bar_at(std::int64_t stamp) { return {100, 105, 95, 101, 1, stamp}; }
std::vector<Bar> chart(int n, int minutes = 1) {
    std::vector<Bar> result;
    for (int i = 0; i < n; ++i)
        result.push_back(bar_at(epoch + static_cast<std::int64_t>(i) * minutes * minute));
    return result;
}
NativeRunSpec spec_for(const char* key, const char* tf = "1") {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = tf;
    spec.script_tf = tf;
    spec.tickerid = "SYN:MATCH";
    spec.ticker = "MATCH";
    spec.type = "stock";
    spec.currency = "USD";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    return spec;
}

class TrailHost final : public NativeStrategyHost {
public:
    std::vector<no::RequestHandle> expected;
    std::size_t callbacks = 0, observations = 0;
    std::size_t warm_capacity = 0, final_capacity = 0;
    explicit TrailHost(std::size_t population) : population_(population) {
        // Exercise the full-book collector, whose contract is all live handles
        // when at least one trail tracks, not only the tracking trails.
        Consumer::bound(*this).set_match_dormant(false);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext& ctx) override {
        const auto step = callbacks++;
        if (step == 0) {
            const auto result = submit(no::Request{no::Transact{static_cast<double>(population_)}, "open", ""});
            CHECK("nativeexecutionconsumer_match_trail_handles", result.handle.has_value());
        }
        if (step == 1) {
            CHECK("nativeexecutionconsumer_match_trail_handles", native_open_lots(100).size() == 1);
            for (std::size_t i = 0; i < population_ + 1; ++i) {
                no::Request request{no::Reduce{no::ExplicitUnits{1}}, "rest", ""};
                if (i < population_) {
                    no::Trail trail;
                    trail.offset = 20; // best<=105 gives stop<=85, below low=95
                    request.trigger = trail;
                } else request.trigger = no::Limit{1000}; // included non-trail
                const auto result = submit(request);
                CHECK("nativeexecutionconsumer_match_trail_handles", result.handle.has_value());
                if (result.handle) expected.push_back(*result.handle);
            }
            CHECK("nativeexecutionconsumer_match_trail_handles", expected.size() == population_ + 1);
        }
        if (step < 3) return; // a full later path has armed the trails
        if (step == 4 && expected.size() == population_ + 1) {
            const auto kept = std::min<std::size_t>(population_, 3);
            for (std::size_t i = kept; i < population_; ++i) {
                const auto result = cancel(expected[i]);
                CHECK("nativeexecutionconsumer_match_trail_handles", result.status == no::CancelStatus::Cancelled);
            }
            expected.erase(expected.begin() + kept, expected.begin() + population_);
        }
        const auto working = native_working_requests();
        CHECK("nativeexecutionconsumer_match_trail_handles", working.size() == expected.size());
        const auto tracking = std::count_if(working.begin(), working.end(), [](const auto& row) {
            return std::holds_alternative<no::TrailTrack>(row.trigger_state);
        });
        const auto expected_trails = step < 4 ? population_ : std::min<std::size_t>(population_, 3);
        CHECK("nativeexecutionconsumer_match_trail_handles", static_cast<std::size_t>(tracking) == expected_trails);
        auto& consumer = Consumer::bound(*this);
        // Invoke the same production collector observe_trails invokes, twice
        // at this real book state. This also checks clear-before-refill reuse.
        NativeDriverPoint point{};
        point.coordinate = ctx.coordinate;
        point.matching = true;
        no::MatchCursor cursor{};
        for (int repeat = 0; repeat < 2; ++repeat) {
            const bool collected = (consumer.*access(CollectTrails{}))(point, cursor, false);
            CHECK("nativeexecutionconsumer_match_trail_handles", collected);
            const auto& actual = consumer.*access(Trails{});
            CHECK("nativeexecutionconsumer_match_trail_handles", actual == expected);
            if (step == 3 && repeat == 0) warm_capacity = actual.capacity();
            CHECK("nativeexecutionconsumer_match_trail_handles", actual.capacity() == warm_capacity);
            final_capacity = actual.capacity();
            ++observations;
        }
    }
private:
    std::size_t population_;
};

void trail_workset() {
    for (const std::size_t population : {16U, 1024U}) {
        std::array<std::size_t, 2> capacities{};
        std::size_t trial = 0;
        for (const int n : {16, 64}) {
            TrailHost host(population);
            CHECK("nativeexecutionconsumer_match_trail_handles", host.configure_native(spec_for("match-trails")).status == NativeSetupStatus::Applied);
            const auto bars = chart(n);
            host.run(bars.data(), n);
            CHECK("nativeexecutionconsumer_match_trail_handles", host.native_state().kind == NativeLifecycleKind::Completed);
            CHECK("nativeexecutionconsumer_match_trail_handles", host.callbacks == static_cast<std::size_t>(n));
            CHECK("nativeexecutionconsumer_match_trail_handles", host.observations == 2 * static_cast<std::size_t>(n - 3));
            CHECK("nativeexecutionconsumer_match_trail_handles", host.expected.size() == 1 + std::min<std::size_t>(population, 3));
            capacities[trial++] = host.final_capacity;
        }
        CHECK("nativeexecutionconsumer_match_trail_handles", capacities[0] == capacities[1]);
    }
}

class CohortHost final : public NativeStrategyHost {
public:
    explicit CohortHost(std::size_t population) : population_(population) {}
    std::vector<no::RequestHandle> selected;
    std::size_t callbacks = 0, observations = 0;
    std::size_t opening_capacity = 0, handles_capacity = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const auto step = callbacks++;
        if (step == 0) {
            cohort_ = cohort_open();
            // A final unrelated lot proves the selection is a filter.
            for (std::size_t i = 0; i <= population_; ++i) {
                const auto result = submit(no::Request{no::Transact{1}, "open", ""});
                CHECK("nativeexecutionconsumer_match_target", result.handle.has_value());
                CHECK("nativeexecutionconsumer_match_target_handles", result.handle.has_value());
                if (result.handle && i < population_) {
                    cohort_add(*cohort_, *result.handle);
                    selected.push_back(*result.handle);
                }
            }
        }
        if (step == 1) {
            CHECK("nativeexecutionconsumer_match_target", native_open_lots(100).size() == population_ + 1);
            CHECK("nativeexecutionconsumer_match_target_handles", selected.size() == population_);
            no::Request request{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "exit", ""};
            request.owner = no::BindCohort{*cohort_};
            request.trigger = no::Limit{1000};
            const auto result = submit(request);
            CHECK("nativeexecutionconsumer_match_target", result.handle.has_value());
            CHECK("nativeexecutionconsumer_match_target_handles", result.handle.has_value());
            exit_ = result.handle;
        }
        if (step < 3 || !exit_) return;
        if (step == 4) {
            const auto kept = std::min<std::size_t>(population_, 3);
            for (std::size_t i = kept; i < selected.size(); ++i)
                cohort_remove(*cohort_, selected[i]);
            selected.resize(kept);
        }
        auto& consumer = Consumer::bound(*this);
        const auto& core = consumer.*access(Requests{});
        const auto live = core.hold_live(*exit_);
        CHECK("nativeexecutionconsumer_match_target", live != nullptr);
        CHECK("nativeexecutionconsumer_match_target_handles", live != nullptr);
        if (!live) return;
        CHECK("nativeexecutionconsumer_match_target", std::holds_alternative<no::CohortClose>(live->authority));
        for (int repeat = 0; repeat < 2; ++repeat) {
            // This is the real matcher's scratch reader. A synthesized run
            // disables the indexed-cohort shortcut, reaching handles.push_back
            // and read_openings_into on every call.
            (void)(consumer.*access(CohortSide{}))(*this, *live);
            const auto& target = consumer.*access(Target{});
            const auto& handles = consumer.*access(TargetHandles{});
            CHECK("nativeexecutionconsumer_match_target", target.openings.size() == selected.size());
            CHECK("nativeexecutionconsumer_match_target_handles", handles == selected);
            for (std::size_t i = 0; i < std::min(target.openings.size(), selected.size()); ++i) {
                CHECK("nativeexecutionconsumer_match_target", target.openings[i].queried_opening == selected[i]);
                CHECK("nativeexecutionconsumer_match_target", target.openings[i].has_live_matching_lot);
                // All entries are bought into the first non-flat episode.
                CHECK("nativeexecutionconsumer_match_target", target.openings[i].queried_cycle == 1);
            }
            if (step == 3 && repeat == 0) {
                opening_capacity = target.openings.capacity();
                handles_capacity = handles.capacity();
            }
            CHECK("nativeexecutionconsumer_match_target", target.openings.capacity() == opening_capacity);
            CHECK("nativeexecutionconsumer_match_target_handles", handles.capacity() == handles_capacity);
            ++observations;
        }
    }
private:
    std::size_t population_;
    std::optional<no::CohortHandle> cohort_;
    std::optional<no::RequestHandle> exit_;
};

void target_worksets() {
    for (const std::size_t population : {1U, 64U, 1024U}) {
        std::array<std::pair<std::size_t, std::size_t>, 2> capacities{};
        std::size_t trial = 0;
        for (const int n : {16, 64}) {
            CohortHost host(population);
            auto spec = spec_for("match-targets");
            spec.intrabar.value = IntrabarPath::synthesized{};
            CHECK("nativeexecutionconsumer_match_target", host.configure_native(spec).status == NativeSetupStatus::Applied);
            const auto bars = chart(n);
            host.run(bars.data(), n);
            CHECK("nativeexecutionconsumer_match_target", host.native_state().kind == NativeLifecycleKind::Completed);
            CHECK("nativeexecutionconsumer_match_target_handles", host.callbacks == static_cast<std::size_t>(n));
            CHECK("nativeexecutionconsumer_match_target", host.observations == 2 * static_cast<std::size_t>(n - 3));
            CHECK("nativeexecutionconsumer_match_target_handles", host.selected.size() == std::min<std::size_t>(population, 3));
            capacities[trial++] = {host.opening_capacity, host.handles_capacity};
        }
        CHECK("nativeexecutionconsumer_match_target", capacities[0].first == capacities[1].first);
        CHECK("nativeexecutionconsumer_match_target_handles", capacities[0].second == capacities[1].second);
    }
}

struct IdleHost final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

void bar_view_slots() {
    const auto bars = chart(2);
    const std::array<std::int64_t, 2> closes{epoch + minute, epoch + 2 * minute};
    const NativeInstrumentFeedView descriptor{{bars.data(), bars.size()}, closes.data()};
    std::array<std::size_t, 2> peaks{};
    std::size_t trial = 0;
    for (const int cycles : {8, 32}) {
        IdleHost host;
        auto& consumer = Consumer::bound(host);
        std::size_t peak = 0;
        std::uint64_t generation = 0;
        for (int cycle = 0; cycle < cycles; ++cycle) {
            for (const std::size_t n : {1U, 4U, 16U, 4U, 1U}) {
                std::vector<const NativeInstrumentFeedView*> slots(n, &descriptor);
                consumer.stage_instrument_feed_views(slots.data(), slots.size());
                const auto& pending = consumer.*access(Pending{});
                CHECK("nativeexecutionconsumer_barviewbinding_slots", pending.slots.size() == n);
                for (std::size_t i = 0; i < pending.slots.size(); ++i) {
                    CHECK("nativeexecutionconsumer_barviewbinding_slots", pending.descriptors[i].has_value());
                    CHECK("nativeexecutionconsumer_barviewbinding_slots", pending.slots[i] == &*pending.descriptors[i]);
                    CHECK("nativeexecutionconsumer_barviewbinding_slots", pending.slots[i]->bars.data == bars.data());
                    CHECK("nativeexecutionconsumer_barviewbinding_slots", pending.slots[i]->bars.size == bars.size());
                }
                // Replacing staged views changes the size, never appends.
                consumer.stage_instrument_feed_views(slots.data(), slots.size());
                CHECK("nativeexecutionconsumer_barviewbinding_slots", (consumer.*access(Pending{})).slots.size() == n);
                auto spec = spec_for("match-views");
                spec.identity.run_number = ++generation;
                for (std::size_t i = 0; i < n; ++i) {
                    NativeInstrumentFeed feed;
                    feed.instrument = "SYN:FEED" + std::to_string(i);
                    feed.tf = "1";
                    spec.instrument_feeds.push_back(std::move(feed));
                }
                CHECK("nativeexecutionconsumer_barviewbinding_slots", host.configure_native(spec).status == NativeSetupStatus::Applied);
                const auto& active = consumer.*access(Active{});
                CHECK("nativeexecutionconsumer_barviewbinding_slots", (consumer.*access(Pending{})).slots.empty());
                CHECK("nativeexecutionconsumer_barviewbinding_slots", active.slots.size() == n);
                for (const auto* slot : active.slots) {
                    CHECK("nativeexecutionconsumer_barviewbinding_slots", slot != nullptr);
                    if (slot) {
                        CHECK("nativeexecutionconsumer_barviewbinding_slots", slot->bars.data == bars.data());
                        CHECK("nativeexecutionconsumer_barviewbinding_slots", slot->close_ms == closes.data());
                    }
                }
                const auto capacity = active.slots.capacity();
                peak = std::max(peak, capacity);
                host.run(bars.data(), static_cast<int>(bars.size()));
                CHECK("nativeexecutionconsumer_barviewbinding_slots", host.native_state().kind == NativeLifecycleKind::Completed);
                CHECK("nativeexecutionconsumer_barviewbinding_slots", active.slots.empty());
                // detach uses clear(): capacity is intentionally retained.
                CHECK("nativeexecutionconsumer_barviewbinding_slots", active.slots.capacity() == capacity);
                CHECK("nativeexecutionconsumer_barviewbinding_slots", (consumer.*access(Pending{})).slots.empty());
                consumer.stage_instrument_feed_views(nullptr, 0);
                CHECK("nativeexecutionconsumer_barviewbinding_slots", (consumer.*access(Pending{})).slots.empty());
                CHECK("nativeexecutionconsumer_barviewbinding_slots", (consumer.*access(Pending{})).slots.capacity() == 0);
            }
        }
        peaks[trial++] = peak;
    }
    CHECK("nativeexecutionconsumer_barviewbinding_slots", peaks[0] == peaks[1]);
}

// The independent tape has 1,2,4,8,1 sub-bars in successive 15-minute
// windows. The last is a zero-volume single print, which produces one sample.
constexpr std::array<int, 5> segment_counts{1, 2, 4, 8, 1};
Bar lower_bar(int script, int sub) {
    Bar bar = bar_at(epoch + static_cast<std::int64_t>(script) * 15 * minute + sub * minute);
    const int count = segment_counts[static_cast<std::size_t>(script) % segment_counts.size()];
    if (sub + 1 == count) {
        bar.open = bar.high = bar.low = bar.close = 100;
        bar.volume = 0;
    }
    return bar;
}
bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && a.open == b.open && a.high == b.high
        && a.low == b.low && a.close == b.close && a.volume == b.volume;
}

class LowerHost final : public NativeStrategyHost {
public:
    std::size_t callbacks = 0, sub_callbacks = 0;
    std::size_t sub_capacity = 0, sample_capacity = 0;
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override { ++sub_callbacks; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const auto index = callbacks++;
        const auto expected = static_cast<std::size_t>(segment_counts[index % segment_counts.size()]);
        const auto& consumer = Consumer::bound(*this);
        const auto& subs = consumer.*access(SubBars{});
        const auto& samples = consumer.*access(Samples{});
        CHECK("nativeexecutionconsumer_intrabar_sub_bars", subs.size() == expected);
        for (std::size_t j = 0; j < std::min(subs.size(), expected); ++j) {
            CHECK("nativeexecutionconsumer_intrabar_sub_bars", subs[j] != nullptr);
            if (subs[j]) CHECK("nativeexecutionconsumer_intrabar_sub_bars", same_bar(*subs[j], lower_bar(static_cast<int>(index), static_cast<int>(j))));
        }
        // Continuous lower paths treat a flat zero-volume bar as one print.
        CHECK("nativeexecutionconsumer_intrabar_samples", samples.size() == 1);
        if (!samples.empty()) CHECK("nativeexecutionconsumer_intrabar_samples", samples.front() == 100);
        if (index == 3) {
            sub_capacity = subs.capacity();
            sample_capacity = samples.capacity();
        }
        if (index >= 3) {
            CHECK("nativeexecutionconsumer_intrabar_sub_bars", subs.capacity() == sub_capacity);
            CHECK("nativeexecutionconsumer_intrabar_samples", samples.capacity() == sample_capacity);
        }
    }
};

void lower_segments() {
    std::array<std::pair<std::size_t, std::size_t>, 2> capacities{};
    std::size_t trial = 0;
    for (const int n : {20, 80}) {
        LowerHost host;
        auto spec = spec_for("match-lower", "15");
        IntrabarPath::lower_tf lower;
        lower.tf = "1";
        std::size_t expected_subs = 0;
        for (int i = 0; i < n; ++i) {
            const int count = segment_counts[static_cast<std::size_t>(i) % segment_counts.size()];
            expected_subs += count;
            for (int j = 0; j < count; ++j) lower.bars.push_back(lower_bar(i, j));
        }
        spec.intrabar.value = std::move(lower);
        CHECK("nativeexecutionconsumer_intrabar_sub_bars", host.configure_native(spec).status == NativeSetupStatus::Applied);
        const auto bars = chart(n, 15);
        host.run(bars.data(), n);
        CHECK("nativeexecutionconsumer_intrabar_sub_bars", host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK("nativeexecutionconsumer_intrabar_sub_bars", host.callbacks == static_cast<std::size_t>(n));
        CHECK("nativeexecutionconsumer_intrabar_sub_bars", host.sub_callbacks == expected_subs);
        capacities[trial++] = {host.sub_capacity, host.sample_capacity};
    }
    CHECK("nativeexecutionconsumer_intrabar_sub_bars", capacities[0].first == capacities[1].first);
    CHECK("nativeexecutionconsumer_intrabar_samples", capacities[0].second == capacities[1].second);
}

class SampleHost final : public NativeStrategyHost {
public:
    explicit SampleHost(int count) : count_(count) {}
    std::size_t callbacks = 0, warm_capacity = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const auto& consumer = Consumer::bound(*this);
        const auto& samples = consumer.*access(Samples{});
        const auto& subs = consumer.*access(SubBars{});
        CHECK("nativeexecutionconsumer_intrabar_samples", subs.size() == 1);
        // One synthetic sub-bar has mean_volume == its own volume, so the
        // weighted sampler's ratio is 1, round(base*ratio) is base, and
        // clamp to [2,64] leaves every tested base count unchanged.
        CHECK("nativeexecutionconsumer_intrabar_samples", samples.size() == static_cast<std::size_t>(count_));
        if (!samples.empty()) {
            CHECK("nativeexecutionconsumer_intrabar_samples", samples.front() == 100);
            CHECK("nativeexecutionconsumer_intrabar_samples", samples.back() == 101);
        }
        if (callbacks == 0) warm_capacity = samples.capacity();
        CHECK("nativeexecutionconsumer_intrabar_samples", samples.capacity() == warm_capacity);
        ++callbacks;
    }
private:
    int count_;
};

void synthesized_samples() {
    for (int sampler = 0; sampler < 3; ++sampler) {
        for (int count = 2; count <= 64; ++count) {
            std::array<std::size_t, 2> capacities{};
            std::size_t trial = 0;
            for (const int n : {4, 16}) {
                SampleHost host(count);
                auto spec = spec_for("match-samples");
                IntrabarPath::synthesized path;
                path.samples = count;
                path.distribution = sampler == 0 ? MagnifierDistribution::ENDPOINTS
                                                 : MagnifierDistribution::UNIFORM;
                path.volume_weighted = sampler == 2;
                spec.intrabar.value = path;
                CHECK("nativeexecutionconsumer_intrabar_samples", host.configure_native(spec).status == NativeSetupStatus::Applied);
                const auto bars = chart(n);
                host.run(bars.data(), n);
                CHECK("nativeexecutionconsumer_intrabar_samples", host.native_state().kind == NativeLifecycleKind::Completed);
                CHECK("nativeexecutionconsumer_intrabar_samples", host.callbacks == static_cast<std::size_t>(n));
                capacities[trial++] = host.warm_capacity;
            }
            CHECK("nativeexecutionconsumer_intrabar_samples", capacities[0] == capacities[1]);
        }
    }
}
} // namespace

int main() {
    scenario("nativeexecutionconsumer_match_trail_handles", trail_workset);
    scenario("nativeexecutionconsumer_match_target", target_worksets);
    scenario("nativeexecutionconsumer_barviewbinding_slots", bar_view_slots);
    scenario("nativeexecutionconsumer_intrabar_sub_bars", lower_segments);
    scenario("nativeexecutionconsumer_intrabar_samples", synthesized_samples);
    std::printf("memory proof matcher_scratch: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
