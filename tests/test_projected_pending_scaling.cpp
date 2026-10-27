// Public runs populate the pending book. A separate raw walk is the oracle;
// the only instrumentation counts actual raw projection calls, not time.
// Bounds are layout estimates from source, not measured.
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using Snapshot = source::PlacementSnapshot;
using Family = source::PineOrderFamily;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
int failures = 0;

#define CHECK(condition) do { if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
} } while (false)

// Existing explicit-instantiation access pattern: no class rewrite or
// mutation hook. The book is populated only by public strategy commands.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Placements { friend auto access(Placements); };
struct Live { friend auto access(Live); };
struct SameBar { friend auto access(SameBar); };
struct Entries { friend auto access(Entries); };
struct Brackets { friend auto access(Brackets); };
struct Coof { friend auto access(Coof); };
struct Shadows { friend auto access(Shadows); };
struct RawReads { friend auto access(RawReads); };
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Live, &Adapter::live_handles_>;
template struct Access<SameBar, &Adapter::pending_same_bar_commands_>;
template struct Access<Entries, &Adapter::pending_entries_>;
template struct Access<Brackets, &Adapter::pending_bracket_legs_>;
template struct Access<Coof, &Adapter::pending_coof_requests_>;
template struct Access<Shadows, &Adapter::source_shadow_pending_>;
template struct Access<RawReads, &Adapter::projected_raw_pending_calls_>;

struct RawRow {
    Snapshot snapshot;
    std::uint64_t incarnation;
};

std::vector<RawRow> raw_rows(const Adapter& adapter) {
    std::vector<RawRow> rows;
    const auto& placements = adapter.*access(Placements{});
    for (const auto& handle : adapter.*access(Live{})) {
        const auto found = placements.find(handle.incarnation);
        if (found != placements.end()) rows.push_back({found->second, handle.incarnation});
    }
    const auto append = [&](const auto& queued) {
        for (const auto& row : queued) rows.push_back({row.snapshot, 0});
    };
    append(adapter.*access(SameBar{}));
    append(adapter.*access(Entries{}));
    append(adapter.*access(Brackets{}));
    append(adapter.*access(Coof{}));
    append(adapter.*access(Shadows{}));
    return rows;
}

bool exit_family(Family family) {
    return family == Family::ExitLimit || family == Family::ExitStop
        || family == Family::ExitTrail;
}

bool same_exit(const Snapshot& a, const Snapshot& b) {
    // Compare the complete origin independently of the product equality.
    return exit_family(a.family) && exit_family(b.family)
        && a.source_id == b.source_id && a.from_entry == b.from_entry
        && a.bracket_origin.incarnation == b.bracket_origin.incarnation
        && a.bracket_origin.run.run_number == b.bracket_origin.run.run_number
        && a.bracket_origin.run.session_key == b.bracket_origin.run.session_key;
}

struct Expected {
    RawRow first;
    source::PineExitLevels levels;
};

std::vector<Expected> oracle(const Adapter& adapter) {
    const auto raw = raw_rows(adapter);
    std::vector<Expected> rows;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bool duplicate = false;
        for (std::size_t j = 0; j < i; ++j)
            if (same_exit(raw[i].snapshot, raw[j].snapshot)) duplicate = true;
        if (duplicate) continue;
        Expected row{raw[i], raw[i].snapshot.exit_levels};
        for (std::size_t j = i + 1; j < raw.size(); ++j) {
            if (!same_exit(raw[i].snapshot, raw[j].snapshot)) continue;
            const auto& levels = raw[j].snapshot.exit_levels;
            const auto take = [](double value, double& out) {
                if (!std::isnan(value)) out = value;
            };
            take(levels.limit, row.levels.limit);
            take(levels.stop, row.levels.stop);
            take(levels.trail_points, row.levels.trail_points);
            take(levels.trail_price, row.levels.trail_price);
            take(levels.trail_offset, row.levels.trail_offset);
            take(levels.profit_ticks, row.levels.profit_ticks);
            take(levels.loss_ticks, row.levels.loss_ticks);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool same_double(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

void check_row(const pf_pending_order_v1_t& got, const Expected& want) {
    const auto& first = want.first.snapshot;
    const std::string id = first.family == Family::Close
        ? "__close__" + first.source_id : first.source_id;
    CHECK(std::strcmp(got.id, id.c_str()) == 0);
    CHECK(std::strcmp(got.from_entry, first.from_entry.c_str()) == 0);
    CHECK(std::strcmp(got.comment, first.comment.c_str()) == 0);
    CHECK(got.incarnation == want.first.incarnation);
    CHECK(got.created_seq == static_cast<std::int64_t>(first.source_sequence));
    CHECK(same_double(got.limit_price, want.levels.limit));
    CHECK(same_double(got.stop_price, want.levels.stop));
    CHECK(same_double(got.trail_points, want.levels.trail_points));
    CHECK(same_double(got.trail_price, want.levels.trail_price));
    CHECK(same_double(got.trail_offset, want.levels.trail_offset));
    CHECK(same_double(got.profit_ticks, want.levels.profit_ticks));
    CHECK(same_double(got.loss_ticks, want.levels.loss_ticks));
}

std::uint64_t enumerate(source::PineStrategyHost& host, Adapter& adapter,
                        const std::vector<Expected>& expected, bool reverse = false) {
    const auto before = adapter.*access(RawReads{});
    std::size_t copied = 0;
    // Exercise the client facade and its usual repeated size() condition.
    for (int i = 0; i < host.observe_pending_count_v1(); ++i) {
        const int index = reverse ? static_cast<int>(expected.size()) - 1 - i : i;
        pf_pending_order_v1_t row{};
        CHECK(host.observe_pending_copy_v1(index, &row) == 0);
        if (index >= 0 && static_cast<std::size_t>(index) < expected.size())
            check_row(row, expected[static_cast<std::size_t>(index)]);
        else CHECK(false);
        ++copied;
    }
    CHECK(copied == expected.size());
    return adapter.*access(RawReads{}) - before;
}

std::uint64_t enumerate_levels(source::PineStrategyHost& host, Adapter& adapter,
                               const std::vector<Expected>& expected) {
    const auto before = adapter.*access(RawReads{});
    std::size_t copied = 0;
    for (int i = 0; i < host.observe_pending_count_v1(); ++i) {
        double stop = nan, limit = nan, trail = nan;
        CHECK(host.observe_pending_effective_levels(i, &stop, &limit, &trail) == 0);
        if (static_cast<std::size_t>(i) < expected.size()) {
            const auto& want = expected[static_cast<std::size_t>(i)].levels;
            CHECK(same_double(stop, want.stop));
            CHECK(same_double(limit, want.limit));
            CHECK(same_double(trail, want.trail_price));
        } else CHECK(false);
        ++copied;
    }
    CHECK(copied == expected.size());
    return adapter.*access(RawReads{}) - before;
}

class ScalingHost final : public source::PineStrategyHost {
public:
    explicit ScalingHost(int count) : count_(count) {
        source::PineStrategyConfig config;
        config.initial_capital = 10'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("held", true, nan, nan, static_cast<double>(count_));
            return;
        }
        if (pine_bar_index() != 1) return;
        // The parent holds count units. Each bracket reserves one unit:
        // count * 1 = count, so no bracket depends on over-reservation.
        // At price 100, stops below 50 and limits above 200 never execute.
        for (int i = 0; i < count_; ++i)
            strategy_exit("exit-" + std::to_string(i), "held",
                          200.0 + i, 50.0 - i / 1000.0, nan, nan, nan, nan,
                          "pending bracket", 1.0);
        const auto expected = oracle(adapter_);
        CHECK(expected.size() == static_cast<std::size_t>(count_));
        CHECK(raw_rows(adapter_).size() == 2 * expected.size());
        calls = enumerate(*this, adapter_, expected);
        std::printf("projected pending rows=%d raw_calls=%llu bound=%llu\n",
                    count_, static_cast<unsigned long long>(calls),
                    static_cast<unsigned long long>(4ULL * count_));
        CHECK(calls > 0);
        if (calls > 4ULL * static_cast<std::uint64_t>(count_))
            std::fputs("FAIL projected pending raw projection calls exceeded linear bound\n", stderr);
        CHECK(calls <= 4ULL * static_cast<std::uint64_t>(count_));
        if (failures != 0) return;
        CHECK(enumerate(*this, adapter_, expected, true) <= 4ULL * count_);
        CHECK(enumerate_levels(*this, adapter_, expected) <= 4ULL * count_);
        observed = true;
    }
    std::uint64_t calls = 0;
    bool observed = false;
private:
    int count_;
};

class MutationHost final : public source::PineStrategyHost {
public:
    void check() {
        const auto expected = oracle(adapter_);
        enumerate(*this, adapter_, expected);
        enumerate(*this, adapter_, expected, true);
        enumerate_levels(*this, adapter_, expected);
        pf_pending_order_v1_t invalid{};
        CHECK(observe_pending_copy_v1(-1, &invalid) == -1);
        CHECK(observe_pending_copy_v1(static_cast<int>(expected.size()), &invalid) == -1);
        const auto hash = broker_state_hash_projection();
        enumerate(*this, adapter_, expected);
        CHECK(broker_state_hash_projection() == hash);
        ++observations;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("parent", true, 50.0, nan, 16.0);
            for (int i = 0; i < 8; ++i)
                strategy_exit("child-" + std::to_string(i), "parent",
                              200.0 + i, 10.0 + i, nan, nan, nan, nan, "", 1.0);
            check();
            strategy_exit("child-3", "parent", nan, 25.0, nan, nan, nan, nan, "", 1.0);
            check(); // reissue changes family and raw membership
            strategy_cancel("child-1");
            check();
            for (int i = 0; i < 80; ++i)
                strategy_order("order-" + std::to_string(i), true, 1.0, 5.0);
            check(); // insertions move vector storage
        } else if (pine_bar_index() == 1) {
            check(); // the pending parent filled; its brackets materialized
            strategy_exit("child-3", "parent", 250.0, 20.0, nan, nan, nan, nan, "", 1.0);
            check();
            strategy_cancel_all();
            check();
            CHECK(observe_pending_count_v1() == 0);
            strategy_entry("new-parent", true, 5.0, nan, 1.0);
            strategy_exit("new-child", "new-parent", 300.0, 1.0);
            check(); // repopulation after clear must not reuse old indexes
        }
    }
    int observations = 0;
};
} // namespace

int main() {
    const Bar bars[] = {{100, 100, 100, 100, 1, 60'000},
                        {100, 100, 100, 100, 1, 120'000}};
    std::uint64_t previous = 0;
    for (const int count : std::array<int, 4>{{500, 1000, 2000, 4000}}) {
        ScalingHost host(count);
        host.run(bars, 2, "1", "1");
        CHECK(host.last_error().empty());
        if (failures != 0) return 1; // a failed bound stops larger workloads
        CHECK(host.observed);
        if (previous != 0) CHECK(host.calls * 10 <= previous * 22);
        previous = host.calls;
        if (failures != 0) return 1;
    }
    MutationHost mutation;
    const Bar fills[] = {{100, 100, 100, 100, 1, 60'000},
                         {50, 50, 50, 50, 1, 120'000}};
    for (int run = 0; run < 2; ++run) {
        const int before = mutation.observations;
        mutation.run(fills, 2, "1", "1");
        CHECK(mutation.last_error().empty());
        CHECK(mutation.observations == before + 8);
    }
    std::printf("projected pending scaling: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
