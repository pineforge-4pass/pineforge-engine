// R11: compile the real adapter with a test-only observation at each
// completed_entry_units query. The old walk below is independent of both the
// certificate and the production fallback. The original L18 T/H/E/metrics
// differential is included verbatim, including its 100k fixture.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_position_tail.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace r11_observer {
using namespace pineforge;
using Work = source::PineExecutionAdapter::PositionTailWork;
bool compare_old_walk = true;
bool force_ordered_replay = false;
std::uint64_t oracle_queries = 0, oracle_rows = 0;
std::uint64_t queries = 0, certified = 0, fallback = 0, updates = 0, replay = 0, origins = 0;
const void* previous_adapter = nullptr;
Work previous{};

void reset_counts() {
    queries = certified = fallback = updates = replay = origins = 0;
    oracle_queries = oracle_rows = 0;
    previous_adapter = nullptr;
    previous = {};
}

template<class Host, class Cohorts>
void observe(double actual, bool fast, const Work& work, const void* adapter,
             const Host& host, std::size_t base, const source::detail::PositionTailSpill* spill,
             std::int64_t barrier, const Cohorts& cohorts, bool ancestry, bool fifo,
             const std::string& name, bool is_long) {
    if (previous_adapter != adapter || work.queries <= previous.queries) previous = {};
    updates += work.update_rows - previous.update_rows;
    replay += work.replay_rows - previous.replay_rows;
    origins += work.origin_visits - previous.origin_visits;
    previous = work;
    previous_adapter = adapter;
    ++queries;
    if (fast) ++certified;
    else ++fallback;
#ifdef PINEFORGE_POSITION_TAIL_WORK_REPORT
    PINEFORGE_POSITION_TAIL_WORK_REPORT(work, adapter, fast);
#endif
    if (!compare_old_walk) return;

    const auto includes = [&](bool side, std::uint64_t incarnation, bool margin,
                              const auto& named) {
        if (side != is_long) return false;
        if (!ancestry) return margin && (!fifo || named());
        for (const auto& cohort : cohorts)
            if ((!fifo || cohort.first == name)
                && cohort.second.live_units_by_origin.count(incarnation)) return true;
        return false;
    };
    double expected = 0.0;
    bool reached_spill = true;
    for (std::size_t index = host.closed_trade_count(); index > base;) {
        const Trade& trade = host.get_trade(static_cast<int>(--index));
        ++oracle_rows;
        if (trade.exit_time < barrier) {
            reached_spill = false;
            break;
        }
        if (includes(trade.is_long, trade.entry_incarnation,
                     trade.exit_id == source::kMarginCallLabel,
                     [&] { return trade.entry_id == name; })) expected += trade.qty;
    }
    if (reached_spill && spill) {
        auto cursor = spill->size_bytes();
        source::detail::PositionTailSpill::Row trade;
        while (spill->previous(cursor, trade)) {
            ++oracle_rows;
            if (trade.exit_time < barrier) break;
            if (includes(trade.is_long, trade.entry_incarnation, trade.margin_call,
                         [&] { return spill->entry_id_equals(trade, name); })) expected += trade.qty;
        }
    }
    ++oracle_queries;
    if (std::memcmp(&actual, &expected, sizeof actual) != 0)
        throw std::runtime_error("R11 adapter completed_entry_units differs from old walk");
}
} // namespace r11_observer

// As in test_adapter_growth_storage.cpp, these definitions satisfy the
// adapter symbols before the static archive is searched. Other source/core
// objects are the normal library. No access specifier or class is changed.
#define PINEFORGE_POSITION_TAIL_OBSERVE(value, fast) \
    ::r11_observer::observe(value, fast, position_tail_work(), this, host, base, \
        position_tail_spill_.get(), position_open_script_bar_, cohorts_by_id_, \
        ancestry_sized_target, fifo_named_entry, from_entry, physical.signed_units > 0.0)
#define PINEFORGE_POSITION_TAIL_BEFORE_QUERY(summary) \
    do { if (::r11_observer::force_ordered_replay) \
        (summary).invalidate(::pineforge::source::PineExecutionAdapter::PositionTailGuard::Invalidated, \
                             PINEFORGE_POSITION_TAIL_DIAGNOSTICS{}); } while (false)
#include "../src/source/pine_adapter.cpp"
#undef PINEFORGE_POSITION_TAIL_BEFORE_QUERY
#undef PINEFORGE_POSITION_TAIL_OBSERVE

#define PINEFORGE_L18_SCALE_OUT_100K
#define main r11_original_scale_main
#include "test_position_tail_scale_out.cpp"
#undef main
#undef PINEFORGE_L18_SCALE_OUT_100K

#ifndef PINEFORGE_POSITION_TAIL_SCALE_MAIN
#define PINEFORGE_POSITION_TAIL_SCALE_MAIN main
#endif
int PINEFORGE_POSITION_TAIL_SCALE_MAIN() {
    try {
        using namespace r11_observer;
        reset_counts();
        // The independent oracle's counted old walk is exactly N^2 for
        // each execution. This witnesses the population and the old cost,
        // without making the 100k run repeat twenty billion reference reads.
        constexpr std::uint64_t small = 1024;
        force_ordered_replay = true;
        differential(static_cast<int>(small), true, false, true);
        require(queries == 3 * (small + 1) && oracle_queries == queries,
                "R11 replay control did not exercise its query population");
        require(replay == 3 * small * small && oracle_rows == replay && certified == 0,
                "R11 old-path control did not witness N^2 row visits");
        std::printf("R11 replay control: %llu queries, %llu replay rows, %llu oracle rows\n",
                    static_cast<unsigned long long>(queries),
                    static_cast<unsigned long long>(replay),
                    static_cast<unsigned long long>(oracle_rows));
        force_ordered_replay = false;
        reset_counts();
        differential(static_cast<int>(small), true, false, true);
        require(oracle_queries == 3 * (small + 1), "R11 did not observe every small query");
        require(oracle_rows == 3 * small * small, "R11 old-walk quadratic witness changed");
        require(replay == 0 && updates <= 8 * small && origins <= 6 * (small + 1),
                "R11 certified small fixture reread history");
        require(fallback == 0 && certified == queries, "R11 integer fixture fell back");
        // Same-bar flat/reopen/reversal and reused entry id, with the oracle
        // active at every internal and ordinary query.
        differential(32, true, true, false);

        reset_counts();
        compare_old_walk = false;
        char name[] = "test_position_tail_summary_scale";
        char* arguments[] = {name, nullptr};
        require(r11_original_scale_main(1, arguments) == 0, "unchanged L18 differential failed");
        constexpr std::uint64_t large = 100000;
        require(queries >= 2 * (large + 1), "R11 100k fixture did not publish its query population");
        require(certified == queries && fallback == 0 && replay == 0,
                "R11 100k fixture used the ordered history walk");
        require(updates > 2 * large && updates <= 8 * large,
                "R11 100k row-read count is not linear");
        require(origins <= 3 * queries, "R11 query walked more than the live origin population");
        std::printf("R11 100k work: %llu queries, %llu update rows, %llu replay rows, "
                    "%llu live-origin visits\n",
                    static_cast<unsigned long long>(queries),
                    static_cast<unsigned long long>(updates),
                    static_cast<unsigned long long>(replay),
                    static_cast<unsigned long long>(origins));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "R11 scale/oracle failure: %s\n", error.what());
        return 1;
    }
    return 0;
}
