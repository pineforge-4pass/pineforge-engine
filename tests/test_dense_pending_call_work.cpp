#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Adapter = pineforge::source::PineExecutionAdapter;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// DenseHost's public source-host run pattern, with alternating sides.
// Hand oracle: every bar lies in [99.5, 100.5]. A buy limit at 1 and a sell
// limit at 1000 cannot fill. Each new id therefore leaves one pending entry
// and no trade; the matching exit also exercises the pending-entry queries.
class DensePendingHost final : public pineforge::source::PineStrategyHost {
public:
    explicit DensePendingHost(int period) : period_(period) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
    }

    void on_source_bar(const pineforge::Bar&) override {
        const int bar = pine_bar_index();
        const auto suffix = std::to_string(period_ == 0 ? bar : bar % period_);
        const auto entry_id = "pending-" + suffix;
        const auto exit_id = "exit-" + suffix;
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (period_ != 0 && bar >= period_) {
            strategy_cancel(exit_id);
            strategy_cancel(entry_id);
        }
        const bool is_long = bar % 2 == 0;
        strategy_entry(entry_id, is_long, is_long ? 1.0 : 1000.0, na, 1.0);
        strategy_exit(exit_id, entry_id, na, is_long ? 50.0 : 150.0);
        ++source_bars_;
    }

    Adapter::PendingScanWork work() const { return adapter_.pending_scan_work(); }
    Adapter::DensePendingWork dense_work() const { return adapter_.dense_pending_work(); }
    int source_bars() const { return source_bars_; }

    // Read the real public working-book snapshot, not a diagnostic maximum.
    // Exit labels use a different prefix, so they cannot inflate this count.
    std::size_t pending_entries() const {
        std::size_t count = 0;
        for (const auto& row : native_working_requests())
            if (row.definition->request.label.compare(0, 8, "pending-") == 0) ++count;
        return count;
    }

private:
    int period_;
    int source_bars_ = 0;
};

struct Measurement {
    Adapter::PendingScanWork scans;
    Adapter::DensePendingWork walks;
};

// A single enumeration drives both tables, all dense ratios and every fixed
// control bound, including every field of the pre-existing scan counters.
template<class Visit>
void visit_counters(const Measurement& measurement, Visit visit) {
    const auto scan = [&](const char* name, const Adapter::PendingScanCounter& work) {
        visit(std::string(name) + ".calls", work.calls);
        visit(std::string(name) + ".live_handle_iterations", work.live_handle_iterations);
        visit(std::string(name) + ".placement_find_calls", work.placement_find_calls);
        visit(std::string(name) + ".open_lot_iterations", work.open_lot_iterations);
        visit(std::string(name) + ".max_live_handles", work.max_live_handles);
        visit(std::string(name) + ".max_placement", work.max_placement);
        visit(std::string(name) + ".max_pending_bracket_legs", work.max_pending_bracket_legs);
        visit(std::string(name) + ".max_resident_cohorts", work.max_resident_cohorts);
        visit(std::string(name) + ".max_logical_cohorts", work.max_logical_cohorts);
    };
    scan("priced_entry_order_working", measurement.scans.priced_entry);
    scan("entry_order_working", measurement.scans.entry);
    scan("standing_exit", measurement.scans.standing_exit);
    scan("open_lot_of", measurement.scans.open_lot);
    using Work = Adapter::DensePendingWork;
    struct Walk {
        const char* name;
        std::uint64_t Work::* counter;
    };
    const Walk walks[] = {
        {"remember.handles", &Work::remember_handles},
        {"retire.handles", &Work::retire_handles},
        {"retire.source_keys", &Work::retire_keys},
        {"origin_is_pending.handles", &Work::origin_pending_handles},
        {"live_origin_positions.handles", &Work::live_origin_handles},
        {"place_entry.prior_closes", &Work::entry_prior_closes},
        {"place_entry.dynamic_exits", &Work::entry_dynamic_exits},
        {"opposite_entry_opening_pending.handles", &Work::entry_opposite_pending},
        {"place_entry.opposite_close_pass", &Work::entry_opposite_close_pass},
        {"place_entry.batch_openings", &Work::entry_batch_openings},
        {"submit_entry_with_policy.reservation_captures", &Work::entry_reservation_captures},
        {"exit.staged_parents", &Work::exit_staged_parents},
        {"exit.reversal_parents", &Work::exit_reversal_parents},
        {"exit.reservation_parents", &Work::exit_reservation_parents},
        {"exit.live_parents", &Work::exit_live_parents},
        {"exit.origin_handles", &Work::exit_origin_handles},
        {"exit.drop_legs", &Work::exit_drop_legs},
        {"submit_or_replace.pending_brackets", &Work::submit_pending_brackets},
        {"compute_exit_reservation.handles", &Work::compute_exit_reservation_handles},
        {"compute_exit_reservation.pending_brackets", &Work::compute_exit_reservation_pending_brackets},
        {"exit.foreign_pending_openings", &Work::exit_foreign_pending_handles},
        {"flush_pending_bracket_legs.prescript_rows", &Work::flush_pending_bracket_legs_prescript_rows},
        {"flush_pending_bracket_legs.sort_comparisons", &Work::flush_pending_bracket_legs_sort_comparisons},
        {"flush_pending_bracket_legs.queued", &Work::flush_pending_bracket_legs_queued},
        {"cancel.token_handles", &Work::cancel_token_handles},
        {"cancel.match_handles", &Work::cancel_match_handles},
        {"stash_id_bound_exits.handles", &Work::stash_id_bound_exits_handles},
        {"retire_cancelled_exits.placements", &Work::retire_cancelled_exits_placements},
        {"withdraw_waypoint_margin_calls.handles", &Work::withdraw_waypoint_margin_calls_handles},
        {"same_bar_close_alls.handles", &Work::same_bar_close_alls_handles},
        {"on_bar_open.trails", &Work::open_trails},
        {"on_bar_open.admission_gate", &Work::open_admission_gate},
        {"apply_open_market_admission.rows", &Work::open_admission_rows},
        {"on_bar_open.stop_gate", &Work::open_stop_gate},
        {"on_bar_open.gapped_gate", &Work::open_gapped_gate},
        {"order_open_marketable_limit_entries.gate", &Work::open_limit_gate},
        {"on_bar_open.touched_stops", &Work::open_touched_stops},
        {"on_bar_open.reversal_gate", &Work::open_reversal_gate},
        {"same_point_two_leg_book.handles", &Work::same_point_two_leg_book_handles},
        {"order_same_point_entries.candidates", &Work::same_point_candidates},
        {"update_l4c_priority.calls", &Work::update_l4c_priority_calls},
        {"refresh_pending_view.calls", &Work::refresh_pending_view_calls},
    };
    for (const auto& walk : walks)
        visit(std::string(walk.name), measurement.walks.*walk.counter);
}

Measurement measure(const char* script, int period, int n) {
    std::vector<pineforge::Bar> bars(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto& bar = bars[static_cast<std::size_t>(i)];
        bar.timestamp = 1736121600000LL + static_cast<std::int64_t>(i) * 60000;
        bar.open = bar.close = 100.0;
        bar.high = 100.5;
        bar.low = 99.5;
        bar.volume = 1000.0;
    }
    DensePendingHost host(period);
    host.run(bars.data(), n, "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.source_bars() == n, "the public run must evaluate every source bar");
    const Measurement result{host.work(), host.dense_work()};
    visit_counters(result, [&](const std::string& name, std::uint64_t value) {
        std::printf("CR391B %s N=%d %s=%llu\n", script, n, name.c_str(),
                    static_cast<unsigned long long>(value));
    });
    std::fflush(stdout);
    if (period == 0) {
        require(host.trade_count() == 0, "unreachable entry limits must produce zero trades");
        // One distinct entry id per executed bar, with no reachable fill or
        // cancellation: pending entries = sum(1 for each source bar) = n.
        require(host.pending_entries() == static_cast<std::size_t>(n),
                "dense pending entry count after the run must equal N");
    }
    return result;
}

void dense_pending_call_work() {
    constexpr std::array<int, 2> sizes{{256, 512}};
    const std::array<Measurement, 2> dense{{
        measure("dense", 0, sizes[0]), measure("dense", 0, sizes[1])}};
    const std::array<Measurement, 2> fixed{{
        measure("fixed", 1, sizes[0]), measure("fixed", 1, sizes[1])}};
    std::vector<std::uint64_t> dense_small, fixed_small;
    visit_counters(dense[0], [&](const std::string&, std::uint64_t value) {
        dense_small.push_back(value);
    });
    visit_counters(fixed[0], [&](const std::string&, std::uint64_t value) {
        fixed_small.push_back(value);
    });
    std::size_t index = 0;
    visit_counters(dense[1], [&](const std::string& name, std::uint64_t large) {
        const auto small = dense_small[index++];
        if (small == 0)
            std::printf("CR391B ratio %s zero\n", name.c_str());
        else
            std::printf("CR391B ratio %s %.2f\n", name.c_str(),
                        static_cast<double>(large) / static_cast<double>(small));
        // Infrastructure only: doubling the commands permits 2.5x work.
        // These four counts include the two retirement walks; zero/positive
        // must fail as well. The other conversion lanes own their bounds.
        if ((name == "remember.handles" || name == "origin_is_pending.handles"
             || name == "retire.handles" || name == "retire.source_keys")
            && large * 2 > small * 5)
            throw std::runtime_error("dense counter " + name + " exceeds 2.5x");
    });
    std::fflush(stdout);
    index = 0;
    visit_counters(fixed[1], [&](const std::string& name, std::uint64_t large) {
        const auto small = fixed_small[index++];
        // Keep every fixed-id control bound. Doubling the bars with the
        // same id population permits 2.5x cumulative work.
        if (large * 2 > small * 5)
            throw std::runtime_error("fixed-id counter " + name + " exceeds 2.5x");
    });
}
} // namespace

int main() {
    try {
        dense_pending_call_work();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "dense pending call work: %s\n", error.what());
        return 1;
    }
}
