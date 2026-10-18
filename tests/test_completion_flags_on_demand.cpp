// CR-150 L4: completion bits have one kernel producer and no per-input store.
// The first use of ScriptBucketCursor is deliberately red on the lane base:
// that type is absent. Keep this test commit separate for the lab's red proof.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

namespace {
using namespace pineforge;
int failures = 0;
int checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kStart = 1740960000000LL; // Monday 2025-03-03, UTC.

std::vector<Bar> chart(int n, int minutes = 1) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double price = 100.0 + i % 7;
        bars.push_back({price, price + 2.0, price - 2.0, price + 1.0, 1.0,
                        kStart + i * minutes * kMinute});
    }
    return bars;
}

NativeRunSpec spec_for(const char* input, const char* script,
                       const char* zone = "UTC", const char* session = "24x7") {
    NativeRunSpec spec;
    spec.identity = {"completion-flags-on-demand", 1};
    spec.input_tf = input;
    spec.script_tf = script;
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "stock";
    spec.currency = "USD";
    spec.timezone = zone;
    spec.session = session;
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    return spec;
}

class NativeProbe final : public NativeStrategyHost {
public:
    std::vector<Bar> delivered;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext&) override {
        delivered.push_back(bar);
    }
};

// Independent reference: the complete pre-L4 vector-producing walk. In
// particular, this is feed(bar), without a next-input timestamp hint.
void preview(const NativeRunSpec& spec, const std::vector<Bar>& bars,
             std::vector<unsigned char>& complete, std::vector<unsigned char>& boundary) {
    TimeframeAggregator aggregator(spec.script_tf, spec.input_tf, spec.timezone, spec.session);
    for (const Bar& bar : bars) {
        const auto aggregate = aggregator.feed(bar);
        complete.push_back(aggregate.is_complete ? 1U : 0U);
        boundary.push_back(aggregate.is_complete
            && tf_change(aggregate.bar.timestamp, bar.timestamp, spec.script_tf,
                         spec.timezone, spec.session) ? 1U : 0U);
    }
}

void cursors_match_the_stored_flags() {
    std::size_t boundary_count = 0;
    for (const auto& pairing : {std::pair<const char*, const char*>{"1", "5"},
                               {"15", "60"}, {"60", "D"}, {"D", "W"}, {"D", "M"}}) {
        for (const char* zone : {"UTC", "America/New_York", "Asia/Tokyo"}) {
            for (const char* session : {"24x7", "0930-1600:23456", "1800-1700:23456"}) {
                const auto spec = spec_for(pairing.first, pairing.second, zone, session);
                const int minutes = std::string(pairing.first) == "D" ? 1440
                    : std::stoi(pairing.first);
                auto bars = chart(241, minutes);
                // Sparse inputs and a partial final bucket exercise lazy boundaries.
                for (std::size_t i = 2; i < bars.size(); ++i)
                    bars[i].timestamp += 7 * minutes * kMinute;
                std::vector<unsigned char> complete, boundary;
                preview(spec, bars, complete, boundary);
                ScriptBucketCursor input(spec);
                ScriptBucketCursor hash_fold(spec);
                BrokerStateHashSink stored_complete, stored_boundary;
                BrokerStateHashSink cursor_complete, cursor_boundary;
                for (std::size_t i = 0; i < bars.size(); ++i) {
                    const auto flags = input.next(bars[i]);
                    CHECK(flags.complete == complete[i]);
                    CHECK(flags.boundary == boundary[i]);
                    boundary_count += flags.boundary;
                    // The hash cursor is intentionally far behind the input cursor.
                    if (i % 17 == 0) {
                        const std::size_t at = hash_fold.position();
                        const auto folded = hash_fold.next(bars[at]);
                        stored_complete.u(complete[at]);
                        stored_boundary.u(boundary[at]);
                        cursor_complete.u(folded.complete);
                        cursor_boundary.u(folded.boundary);
                        CHECK(cursor_complete.h == stored_complete.h);
                        CHECK(cursor_boundary.h == stored_boundary.h);
                    }
                }
                CHECK(input.position() == bars.size());
                CHECK(hash_fold.position() < input.position() / 8);
                // A shorter consumed prefix restarts the fold, independent of input.
                hash_fold.reset();
                CHECK(hash_fold.position() == 0);
                for (std::size_t i = 0; i < 3; ++i) {
                    const auto flags = hash_fold.next(bars[i]);
                    CHECK(flags.complete == complete[i]);
                    CHECK(flags.boundary == boundary[i]);
                }
                NativeProbe host;
                CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
                std::vector<unsigned char> got_complete, got_boundary;
                CHECK(NativeExecutionConsumer::bound(host).script_bucket_completions(
                    bars.data(), bars.size(), got_complete, got_boundary));
                CHECK(got_complete == complete);
                CHECK(got_boundary == boundary);
            }
        }
    }
    CHECK(boundary_count > 0);
}

std::size_t watched_size = 0;
std::size_t watched_allocations = 0;
void count_store(std::size_t bytes) {
    if (bytes == watched_size) ++watched_allocations;
}

class SourceProbe final : public source::PineStrategyHost {
public:
    std::vector<std::uint64_t> hashes;
    SourceProbe() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        source::PineStrategyConfig config;
        config.src_series_active = false;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        BrokerStateHashSink first, second;
        scheduler_.hash_state(first);
        scheduler_.hash_state(second);
        CHECK(first.h == second.h);
        hashes.push_back(first.h);
    }
};

void scheduler_has_no_flag_arrays_and_restarts_hashes() {
    const auto bars = chart(3001);
    for (const char* script : {"1", "15"}) {
        SourceProbe host;
        host.set_broker_state_hash_recording(true);
        // A flag array requests exactly n bytes. Other per-bar stores are
        // outside this assertion; the missing L3/sink work has its own lanes.
        watched_size = bars.size();
        watched_allocations = 0;
        global_allocation::on_recorded = count_store;
        global_allocation::recording = true;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", script);
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        CHECK(host.last_error().empty());
        CHECK(watched_allocations == 0);
        CHECK(!host.hashes.empty());
        const auto first_run = host.hashes;
        host.hashes.clear();
        host.run(bars.data(), static_cast<int>(bars.size()), "1", script);
        CHECK(host.last_error().empty());
        CHECK(host.hashes == first_run);
        SourceProbe prefix;
        prefix.run(bars.data(), 1500, "1", script);
        CHECK(prefix.last_error().empty());
        CHECK(!prefix.hashes.empty());
        CHECK(prefix.hashes.size() <= first_run.size());
        if (prefix.hashes.size() <= first_run.size())
            CHECK(std::equal(prefix.hashes.begin(), prefix.hashes.end(), first_run.begin()));
    }
}

bool same_bar(const Bar& left, const Bar& right) {
    return left.timestamp == right.timestamp && left.open == right.open
        && left.high == right.high && left.low == right.low
        && left.close == right.close && left.volume == right.volume;
}

void subscriptions_borrow_the_forward_look() {
    // Friday's half-day ends before the declared session. Its known Monday
    // successor completes D on Friday; the last row still has successor 0.
    auto spec = spec_for("15", "15", "UTC", "0930-1600:23456");
    spec.subscriptions.push_back({"D", {}, false, false});
    std::vector<Bar> bars;
    for (int day : {4, 7}) {
        const int count = day == 4 ? 14 : 26;
        for (int i = 0; i < count; ++i) {
            const double price = 100.0 + i;
            bars.push_back({price, price, price, price, 1.0,
                kStart + day * 86400000LL + (570 + i * 15) * kMinute});
        }
    }
    TimeframeAggregator reference("D", "15", spec.timezone, spec.session);
    std::vector<Bar> expected;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto result = reference.feed(bars[i], i + 1 < bars.size()
            ? bars[i + 1].timestamp : 0);
        if (result.is_complete) expected.push_back(result.bar);
    }
    CHECK(expected.size() == 2);
    for (bool lookahead : {false, true}) {
        spec.subscriptions[0].lookahead = lookahead;
        NativeProbe host;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
        CHECK(host.last_error().empty());
        CHECK(host.delivered.size() == expected.size());
        for (std::size_t i = 0; i < std::min(host.delivered.size(), expected.size()); ++i)
            CHECK(same_bar(host.delivered[i], expected[i]));

        NativeProbe stream;
        CHECK(stream.configure_native(spec).status == NativeSetupStatus::Applied);
        bool began = false;
        {
            const std::vector<Bar> warmup(bars.begin(), bars.begin() + 14);
            began = stream.stream_begin(warmup.data(), static_cast<int>(warmup.size()),
                                        "15", "15");
        } // Caller warmup storage is gone before any live input arrives.
        CHECK(began);
        if (began) {
            for (std::size_t i = 14; i < bars.size(); ++i)
                CHECK(stream.stream_push_bar(bars[i]));
            CHECK(stream.stream_end(false));
            CHECK(stream.last_error().empty());
            CHECK(stream.delivered.size() == expected.size());
            for (std::size_t i = 0; i < std::min(stream.delivered.size(), expected.size()); ++i)
                CHECK(same_bar(stream.delivered[i], expected[i]));
        }
    }
    // Isolate the old n * sizeof(int64_t) begin allocation on a non-projected
    // subscription. Inputs and script bars have different counts here.
    const auto many = chart(3001);
    auto large = spec_for("1", "15");
    large.subscriptions.push_back({"60", {}, false, false});
    NativeProbe host;
    CHECK(host.configure_native(large).status == NativeSetupStatus::Applied);
    watched_size = many.size() * sizeof(std::int64_t);
    watched_allocations = 0;
    global_allocation::on_recorded = count_store;
    global_allocation::recording = true;
    host.run(many.data(), static_cast<int>(many.size()), "1", "15");
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    CHECK(host.last_error().empty());
    CHECK(watched_allocations == 0);
}
} // namespace

int main() {
    cursors_match_the_stored_flags();
    scheduler_has_no_flag_arrays_and_restarts_hashes();
    subscriptions_borrow_the_forward_look();
    std::printf("CR-150 L4: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
