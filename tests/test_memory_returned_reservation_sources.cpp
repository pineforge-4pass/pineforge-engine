// compat_pine_select_reservation_growth_sources_selected: the real reservation selector's returned buffer.
// Expectations are derived from the submitted orders, not recorded output.
// The same four-bar submit/fill/exit script runs for N and 4N bars.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"

#include <pineforge/compat/pine/reservation_expansion.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using Candidate = compat::pine::ReservationGrowthCandidate;

int checks = 0;
int failures = 0;
const char* scenario = "dispatch";
int observed_bar = -1;

// A failed check records its row and continues; no assert/abort/throw here.
#define MEM_CHECK(expression) do {                                             \
    ++checks;                                                                \
    if (!(expression)) {                                                      \
        ++failures;                                                          \
        std::fprintf(stderr, "FAIL [compat_pine_select_reservation_growth_sources_selected] %s:%d: %s "                   \
                     "(case=%s, bar=%d)\n", __FILE__, __LINE__,               \
                     #expression, scenario, observed_bar);                   \
    }                                                                        \
} while (false)

// Existing repository access pattern; no changed product class definition.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct PendingEntries { friend auto access(PendingEntries); };
struct SameBarCommands { friend auto access(SameBarCommands); };
struct LiveHandles { friend auto access(LiveHandles); };
struct Placements { friend auto access(Placements); };
template struct Access<PendingEntries, &Adapter::pending_entries_>;
template struct Access<SameBarCommands, &Adapter::pending_same_bar_commands_>;
template struct Access<LiveHandles, &Adapter::live_handles_>;
template struct Access<Placements, &Adapter::placement_>;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kCycleBars = 4;
constexpr int kN = 32;
constexpr std::int64_t kStartMs = 1'736'121'600'000LL;

struct Recording {
    const bool previous = global_allocation::recording;
    Recording() { global_allocation::recording = true; }
    ~Recording() { global_allocation::recording = previous; }
};

struct Observation {
    std::size_t count = 0;
    std::size_t bytes_after = 0;
};

// Input vectors, strings and reference data exist BEFORE recording starts.
// Only the production selector and destruction of its return value allocate
// inside this window. Do not reset live counters between windows: that would
// hide a retained block (and invalidate later deallocation accounting).
Observation observe_return(const std::vector<Candidate>& candidates,
                           std::size_t expected_count, int bar,
                           PositionSide side, bool process_on_close = true,
                           bool flat = false, double percent = 100.0,
                           const std::string& from_entry = {}) {
    const auto bytes_before = global_allocation::live_bytes;
    const auto blocks_before = global_allocation::live_blocks;
    MEM_CHECK(!global_allocation::recording);
    MEM_CHECK(bytes_before == 0);
    MEM_CHECK(blocks_before == 0);
    Observation observed;
    {
        Recording recording;
        {
            const auto selected = compat::pine::select_reservation_growth_sources(
                candidates, from_entry, process_on_close, flat, percent, bar, side);
            observed.count = selected.size();
            MEM_CHECK(selected.size() == expected_count);
            MEM_CHECK(selected.size() <= candidates.size());
            // The eligible script submitted K requests, each once. There is
            // no history in this independent expected count. Also compare
            // input identities in order; staged requests can legitimately be 0.
            if (expected_count == candidates.size()) {
                const auto common = std::min(selected.size(), candidates.size());
                for (std::size_t i = 0; i < common; ++i) {
                    MEM_CHECK(selected[i] == candidates[i].incarnation);
                }
            }
            // One vector owns capacity() uint64_t objects. No guessed vector
            // growth factor or platform object-layout constant is needed.
            MEM_CHECK(global_allocation::live_bytes
                     == bytes_before + selected.capacity() * sizeof(std::uint64_t));
            MEM_CHECK(global_allocation::live_blocks
                     == blocks_before + (selected.capacity() == 0 ? 0U : 1U));
        } // The caller drops the actual returned vector here.
    }
    observed.bytes_after = global_allocation::live_bytes;
    MEM_CHECK(observed.bytes_after == bytes_before);
    MEM_CHECK(global_allocation::live_blocks == blocks_before);
    MEM_CHECK(observed.bytes_after == 0);
    return observed;
}

// Read the three actual caller inputs, rather than inventing an adapter
// population. Fact extraction follows initialize_l4c_policy at the base;
// the expected COUNT below comes separately from the script's K submissions.
std::vector<Candidate> caller_candidates(Adapter& adapter) {
    std::vector<Candidate> result;
    const auto collect = [&](const source::PlacementSnapshot& snapshot,
                             std::uint64_t incarnation) {
        if (!snapshot.opening
            || (snapshot.family != source::PineOrderFamily::Entry
                && snapshot.family != source::PineOrderFamily::Order)) {
            return;
        }
        Candidate fact;
        fact.incarnation = incarnation;
        fact.source_id = snapshot.source_id;
        const auto positive = [](double value) {
            return value > 0.0 && value < std::numeric_limits<double>::infinity();
        };
        fact.market_entry = snapshot.family == source::PineOrderFamily::Entry
            && !positive(snapshot.exit_levels.limit)
            && !positive(snapshot.exit_levels.stop)
            && !positive(snapshot.exit_levels.trail_offset)
            && !positive(snapshot.exit_levels.trail_price);
        fact.from_fill = snapshot.birth.from_fill();
        fact.at_entry_capacity = snapshot.projection_over_pyramiding;
        fact.is_long = snapshot.is_long;
        fact.created_position_side =
            static_cast<PositionSide>(snapshot.projection_position_side);
        fact.created_bar = snapshot.projection_created_bar;
        result.push_back(std::move(fact));
    };
    for (const auto& pending : adapter.*access(PendingEntries{})) {
        collect(pending.snapshot, 0);
    }
    for (const auto& pending : adapter.*access(SameBarCommands{})) {
        collect(pending.snapshot, 0);
    }
    const auto& placements = adapter.*access(Placements{});
    for (const auto& handle : adapter.*access(LiveHandles{})) {
        const auto found = placements.find(handle.incarnation);
        if (found != placements.end()) collect(found->second, handle.incarnation);
    }
    return result;
}

struct RunResult {
    std::size_t last_count = 0;
    std::size_t retained_bytes = 0;
    int cycles = 0;
};

class ReservationScript final : public source::PineStrategyHost {
public:
    ReservationScript(int adds, bool is_long) : adds_(adds), is_long_(is_long) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        // One seed + K additions, with one spare slot: no capacity rejection.
        config.pyramiding = adds + 2;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
    }

    RunResult result;

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        observed_bar = bar;
        const auto side = is_long_ ? PositionSide::LONG : PositionSide::SHORT;
        const double direction = is_long_ ? 1.0 : -1.0;
        switch (bar % kCycleBars) {
        case 0:
            MEM_CHECK(live_position_size() == 0.0);
            MEM_CHECK(pending_order_count() == 0);
            strategy_entry("seed", is_long_, kNaN, kNaN, 1.0);
            break;
        case 1: {
            // The preceding close filled the seed. Today's K market orders
            // are still pending when the global full exit is requested.
            MEM_CHECK(live_position_size() == direction);
            for (int add = 0; add < adds_; ++add) {
                strategy_entry("add-" + std::to_string(add), is_long_, kNaN, kNaN, 1.0);
            }
            const auto candidates = caller_candidates(adapter_);
            MEM_CHECK(candidates.size() == static_cast<std::size_t>(adds_));
            for (const auto& candidate : candidates) {
                MEM_CHECK(candidate.created_bar == bar);
                MEM_CHECK(candidate.market_entry && !candidate.from_fill
                         && !candidate.at_entry_capacity);
                MEM_CHECK(candidate.is_long == is_long_);
                MEM_CHECK(candidate.created_position_side == side);
            }
            const auto eligible = observe_return(
                candidates, static_cast<std::size_t>(adds_), bar, side);
            result.last_count = eligible.count;
            result.retained_bytes = eligible.bytes_after;

            // The full current population is rejected if its LAST member
            // does not qualify.
            MEM_CHECK(!candidates.empty());
            if (!candidates.empty()) {
                auto rejected = candidates;
                rejected.back().from_fill = true;
                const auto aborted = observe_return(rejected, 0, bar, side);
                MEM_CHECK(aborted.count == 0);
            }
            // Age and call-level gates must not return prior-bar identities.
            const auto aged = observe_return(candidates, 0, bar + 1, side);
            MEM_CHECK(aged.count == 0);
            const auto partial = observe_return(candidates, 0, bar, side, true, false, 50.0);
            MEM_CHECK(partial.count == 0);
            const auto no_close = observe_return(candidates, 0, bar, side, false);
            MEM_CHECK(no_close.count == 0);
            const auto flat = observe_return(candidates, 0, bar, side, true, true);
            MEM_CHECK(flat.count == 0);
            const std::string entry_scope = "seed";
            const auto scoped = observe_return(
                candidates, 0, bar, side, true, false, 100.0, entry_scope);
            MEM_CHECK(scoped.count == 0);

            // Exercise the real adapter caller as well as inspecting the
            // returned object through the public selector function above.
            strategy_exit("global", "", is_long_ ? 110.0 : 90.0, kNaN,
                          kNaN, kNaN, kNaN, 100.0);
            bool saw_exit = false;
            for (const auto& pending : adapter_.fixture_pending_snapshots()) {
                if (pending.snapshot.source_id != "global") continue;
                saw_exit = true;
                MEM_CHECK(pending.snapshot.reservation_expansion.capture().has_value());
                MEM_CHECK(pending.snapshot.pooc_global_full_exit_tracks_bound_adds);
            }
            MEM_CHECK(saw_exit);
            break;
        }
        case 2:
            // The tape touches the target on this bar. The K additions filled
            // at the preceding close, so all K+1 units must now be closed.
            MEM_CHECK(live_position_size() == 0.0);
            MEM_CHECK(pending_order_count() == 0);
            ++result.cycles;
            MEM_CHECK(trade_count() == result.cycles * (adds_ + 1));
            break;
        case 3:
            MEM_CHECK(live_position_size() == 0.0);
            MEM_CHECK(pending_order_count() == 0);
            break;
        }
    }

private:
    const int adds_;
    const bool is_long_;
};

std::vector<Bar> tape(int count, bool is_long) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const bool hits_target = i % kCycleBars == 2;
        bars.push_back({100.0, hits_target && is_long ? 111.0 : 101.0,
                        hits_target && !is_long ? 89.0 : 99.0, 100.0, 1.0,
                        kStartMs + static_cast<std::int64_t>(i) * 60'000});
    }
    return bars;
}

RunResult run_script(int count, int adds, bool is_long) {
    const auto bars = tape(count, is_long);
    ReservationScript host(adds, is_long);
    host.run(bars.data(), count);
    MEM_CHECK(host.last_error().empty());
    MEM_CHECK(host.result.cycles == count / kCycleBars);
    MEM_CHECK(host.result.last_count == static_cast<std::size_t>(adds));
    MEM_CHECK(host.result.retained_bytes == 0);
    MEM_CHECK(global_allocation::live_bytes == 0);
    return host.result;
}

void reservation_sources_case(int adds, bool is_long) {
    const auto n = run_script(kN, adds, is_long);
    const auto four_n = run_script(4 * kN, adds, is_long);
    // Independent arithmetic: four bars per completed cycle, one seed and
    // K additions per cycle. At the last selector call, exactly K current
    // additions exist for both histories. The caller retains zero bytes.
    MEM_CHECK(n.last_count == four_n.last_count);
    MEM_CHECK(n.last_count == static_cast<std::size_t>(adds));
    MEM_CHECK(four_n.last_count == static_cast<std::size_t>(adds));
    MEM_CHECK(n.retained_bytes == four_n.retained_bytes);
    MEM_CHECK(n.retained_bytes == 0 && four_n.retained_bytes == 0);
}

struct Case {
    const char* row;
    const char* name;
    int adds;
    bool is_long;
    void (*test)(int, bool);
};
} // namespace

int main() {
    // Row dispatch is intentionally non-short-circuiting, including after an
    // unexpected exception. All scenarios retain their descriptive check name in messages.
    const Case cases[] = {
        {"compat_pine_select_reservation_growth_sources_selected", "long-three", 3, true, reservation_sources_case},
        {"compat_pine_select_reservation_growth_sources_selected", "long-seven", 7, true, reservation_sources_case},
        {"compat_pine_select_reservation_growth_sources_selected", "short-three", 3, false, reservation_sources_case},
        {"compat_pine_select_reservation_growth_sources_selected", "short-seven", 7, false, reservation_sources_case},
    };
    for (const auto& entry : cases) {
        scenario = entry.name;
        observed_bar = -1;
        std::printf("RUN [%s] %s\n", entry.row, entry.name);
        try {
            entry.test(entry.adds, entry.is_long);
        } catch (const std::exception& error) {
            ++checks;
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s:%d: unexpected exception: %s (case=%s)\n",
                         entry.row, __FILE__, __LINE__, error.what(), entry.name);
        } catch (...) {
            ++checks;
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s:%d: unexpected exception (case=%s)\n",
                         entry.row, __FILE__, __LINE__, entry.name);
        }
    }
    std::printf("memory proof returned_reservation_sources: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
