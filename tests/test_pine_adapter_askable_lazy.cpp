// Observe the actual adapter object, including when replayed with the eager
// implementation. The standard algorithms still receive their original
// iterators and arguments; only calls on the origin scratch vector are counted.
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_input_domain.hpp>
#include <pineforge/source/magnifier_intrabars.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/compat/pine/market_admission.hpp>
#include <pineforge/compat/pine/trail_ticks.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/timeframe.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/exit_payload_readback.hpp>
#include "../src/engine_internal.hpp"
#include "../src/engine_value_invalid.hpp"
#include "../src/native_execution_consumer.hpp"
#include "../src/timezone.hpp"
#include "../src/scratch_table_binders.hpp"
#include "../src/source/pine_host_reads.hpp"
#include "../src/source/pine_placement_retention.hpp"
#include "../src/source/pine_quiet_bar.hpp"
#include "../src/compat/pine/callback_lifecycle_rules.hpp"
#include "../src/source/pine_reissue_binding.hpp"
#include "../src/source/pine_run_failure.hpp"
#include "../src/source/pine_checked_arith.hpp"
#include "../src/source/pine_command_scratch_production.hpp"
#include "../src/source/exit_scratch_versions_production.hpp"
#include "../src/source/pine_value_admission.hpp"
#include "../src/source/pine_position_tail.hpp"
#include "../src/source/pine_position_tail_binding.hpp"
#include "../src/source/pine_position_tail_summary.hpp"
#include "../src/source/pine_source_key_test.hpp"

// Load the adapter's headers before the narrowly scoped algorithm observers.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace askable_fixture {
using namespace pineforge;
namespace ps = pineforge::source;
using Origins = std::vector<std::uint64_t>;
using Row = ps::PlacementSnapshot;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Work {
    const Origins* origins = nullptr;
    bool reference = false;
    bool matrix = false;
    std::uint64_t sweeps = 0, builds = 0, searches = 0, calls = 0;
    std::uint64_t sweep_builds = 0, shared_build_sweeps = 0;
    std::uint64_t sweep_searches = 0, matrix_searches = 0;
    std::uint64_t observed = 0, outer_rows = 0, inner_rows = 0;
    std::uint64_t retained = 0, released = 0, representatives = 0;
};
inline Work* active = nullptr;

// C++17 equivalent of obtaining a vector iterator's underlying pointer,
// without dereferencing an empty range or comparing unrelated iterators.
template<class Iterator>
auto address(Iterator first) {
    if constexpr (std::is_pointer_v<Iterator>) return first;
    else return address(first.operator->());
}

template<class Iterator>
bool origin_range(Iterator first) {
    if constexpr (std::is_same_v<Iterator, Origins::iterator>
                  || std::is_same_v<Iterator, Origins::const_iterator>) {
        return active && active->origins
            && address(first) == active->origins->data();
    } else {
        return false;
    }
}

template<class Iterator>
Iterator sorted_first(Iterator first) {
    if (origin_range(first)) {
        ++active->builds;
        ++active->sweep_builds;
        if (active->sweeps != 0)
            require(active->sweep_builds == 1, "origins rebuilt within one sweep");
    }
    return first;
}

template<class Iterator>
Iterator searched_first(Iterator first) {
    if (origin_range(first)) {
        if (active->matrix) ++active->matrix_searches;
        else {
            ++active->searches;
            ++active->sweep_searches;
        }
    }
    return first;
}

void next_sweep() {
    if (!active) return;
    if (active->sweep_builds == 1 && active->sweep_searches > 1)
        ++active->shared_build_sweeps;
    ++active->sweeps;
    active->sweep_builds = active->sweep_searches = 0;
}

bool exit_family(ps::PineOrderFamily family) {
    return family == ps::PineOrderFamily::ExitLimit
        || family == ps::PineOrderFamily::ExitStop
        || family == ps::PineOrderFamily::ExitTrail;
}

bool old_withdrawn(const exit_legs::Lifecycle& legs) {
    return legs.retired(exit_legs::Leg::Stop)
        && legs.retired(exit_legs::Leg::Limit)
        && legs.retired(exit_legs::Leg::Trail);
}

bool old_filled_limit(const Row& row) {
    if (!row.filled_exit || row.family != ps::PineOrderFamily::ExitLimit
        || row.from_entry.empty() || row.bracket_origin.incarnation != 0
        || row.per_entry_origin != 0 || row.pending_bound_exit
        || row.legs.pending_replacement()) return false;
    const auto unprotected = [](const exit_legs::Prices& prices) {
        return std::isnan(prices.stop_price) && std::isnan(prices.loss_ticks)
            && std::isnan(prices.trail_points) && std::isnan(prices.trail_price)
            && std::isnan(prices.trail_offset);
    };
    const auto& levels = row.exit_levels;
    const auto& suspension = row.legs.suspension();
    return std::isnan(levels.stop) && std::isnan(levels.loss_ticks)
        && std::isnan(levels.trail_points) && std::isnan(levels.trail_price)
        && std::isnan(levels.trail_offset) && unprotected(row.legs.prices())
        && (!suspension || !suspension->revival_definition
            || unprotected(suspension->revival_definition->prices()));
}

template<class Live, class Cohorts>
bool old_askable(std::uint64_t origin, const Live& live, const Cohorts& cohorts) {
    // Straight walks of the original inputs, independent of the lazy vector.
    for (const auto& handle : live)
        if (handle.incarnation == origin) return true;
    for (const auto& cohort : cohorts)
        for (const auto& opening : cohort.second.opened)
            if (opening.incarnation == origin) return true;
    return false;
}

template<class Live, class Cohorts>
bool old_representative(const Row& row, const ps::PlacementTable& rows,
                        const Live& live, const Cohorts& cohorts) {
    if (!exit_family(row.family) || !row.legs.dormant()
        || row.legs.target().incarnation == 0
        || !old_askable(row.bracket_origin.incarnation, live, cohorts)) return false;
    for (const auto& peer : rows) {
        const auto& other = peer.second;
        if (other.family == row.family && other.bracket_origin == row.bracket_origin
            && other.source_id == row.source_id && other.from_entry == row.from_entry
            && other.legs.dormant() && other.legs.target().incarnation != 0)
            return &other == &row;
    }
    return false;
}

// Independent eager predicate, including both non-lifecycle fallbacks.
// No production predicate/helper or sorted-origin lookup supplies its result.
bool old_pin(const Row& row, std::int64_t cycle, int interval,
             std::int64_t script_open, bool askable, bool representative) {
    const bool eager_askable = askable;
    if (row.legs.target().incarnation != 0 && !old_filled_limit(row)
        && ((row.placement_cycle >= cycle && !old_withdrawn(row.legs))
            || (eager_askable && !row.legs.dormant()) || representative)) return true;
    if (row.family == ps::PineOrderFamily::Close && row.immediately
        && row.projection_created_bar >= interval) return true;
    return (row.family == ps::PineOrderFamily::Entry
            || row.family == ps::PineOrderFamily::Order)
        && row.placement_script_open_ms >= script_open;
}

template<class Actual, class Live, class Cohorts>
bool select_pin(const Row& row, const Actual& actual, std::int64_t cycle, int interval,
                std::int64_t script_open, const ps::PlacementTable& rows,
                const Live& live, const Cohorts& cohorts) {
    if (!active) return actual();
    ++active->calls;
    const bool expected = old_pin(row, cycle, interval, script_open,
        old_askable(row.bracket_origin.incarnation, live, cohorts),
        old_representative(row, rows, live, cohorts));
    if (active->reference) return expected;
    const bool observed = actual();
    require(observed == expected, "self pin differs from independent eager predicate");
    return observed;
}

template<class Pin>
void outer_truth_table(const Pin& pin, std::int64_t cycle, int interval,
                       std::int64_t script_open) {
    // All 2^3 values of target, not-filled-limit, and the inner disjunction.
    // These local values never enter the placement table or native book.
    for (unsigned mask = 0; mask != 8; ++mask) {
        const bool target = (mask & 4) != 0;
        const bool revival = (mask & 2) != 0;
        const bool inner = (mask & 1) != 0;
        Row row;
        row.family = ps::PineOrderFamily::ExitLimit;
        row.from_entry = "entry";
        row.filled_exit = !revival;
        row.placement_cycle = inner ? cycle : cycle - 1;
        if (target) row.legs.attach(1, 0);
        require(!old_filled_limit(row) == revival, "outer matrix filled-limit precondition");
        const auto before = active->matrix_searches;
        const bool expected = old_pin(row, cycle, interval, script_open, false, false);
        require(expected == (target && revival && inner), "outer matrix reference precondition");
        const bool observed = pin(row);
        const std::uint64_t needed = target && revival && !inner ? 1 : 0;
        std::printf("outer mask=%u result=%d/%d searches=%llu/%llu\n", mask,
                    observed ? 1 : 0, expected ? 1 : 0,
                    static_cast<unsigned long long>(active->matrix_searches - before),
                    static_cast<unsigned long long>(needed));
        require(observed == expected, "outer conjunction truth table differs");
        require(active->matrix_searches - before == needed,
                "outer conjunction searched before a decisive cheap check");
        ++active->outer_rows;
    }
}

template<class Pin>
void inner_truth_table(const Pin& pin, const Origins& origins, std::int64_t cycle,
                       int interval, std::int64_t script_open) {
    require(!origins.empty(), "inner matrix needs a live or opened origin");
    require(std::find(origins.begin(), origins.end(), 0) == origins.end(),
            "zero must not be an askable origin");
    // Also cover all cycle/askable/non-dormant combinations, including a
    // non-dormant askable leg whose placement cycle has ended.
    for (unsigned mask = 0; mask != 8; ++mask) {
        const bool current = (mask & 4) != 0;
        const bool askable = (mask & 2) != 0;
        const bool nondormant = (mask & 1) != 0;
        Row row;
        row.family = ps::PineOrderFamily::ExitStop;
        row.placement_cycle = current ? cycle : cycle - 1;
        row.bracket_origin.incarnation = askable ? origins.front() : 0;
        row.legs.attach(1, 0);
        if (!nondormant) {
            const exit_legs::Frame frame{1, interval, exit_legs::Domain::Ordinary,
                                         exit_legs::Phase::Observation};
            const exit_legs::Action suspend{row.legs.target(), row.legs.revision(), frame,
                exit_legs::Suspend{{exit_legs::Leg::Stop}, {}, {}, {}}};
            require(row.legs.apply(row.legs.target(), suspend) == exit_legs::Result::Applied,
                    "inner matrix suspension precondition");
        }
        const auto before = active->matrix_searches;
        const bool expected = old_pin(row, cycle, interval, script_open, askable, false);
        const bool observed = pin(row);
        std::printf("inner mask=%u result=%d/%d searches=%llu/%u\n", mask,
                    observed ? 1 : 0, expected ? 1 : 0,
                    static_cast<unsigned long long>(active->matrix_searches - before),
                    !current && nondormant ? 1U : 0U);
        require(observed == expected, "inner disjunction truth table differs");
        require(active->matrix_searches - before == (!current && nondormant ? 1U : 0U),
                "origin search ran before the cycle or dormancy check");
        ++active->inner_rows;
    }
}

template<class Live, class Cohorts, class Pin>
void observe(const ps::PlacementTable& rows, std::int64_t cycle, const Origins& origins,
             const std::vector<const Row*>& pins, const Live& live, const Cohorts& cohorts,
             int interval, std::int64_t script_open, const Pin& pin) {
    if (!active) return;
    ++active->observed;
    Origins expected;
    const auto add = [&](std::uint64_t incarnation) {
        if (std::find(expected.begin(), expected.end(), incarnation) == expected.end())
            expected.push_back(incarnation);
    };
    for (const auto& handle : live) add(handle.incarnation);
    for (const auto& cohort : cohorts)
        for (const auto& opening : cohort.second.opened) add(opening.incarnation);
    std::sort(expected.begin(), expected.end());
    require(origins == expected, "sorted unique origins differ from raw input walks");
    std::vector<const Row*> expected_pins;
    for (const auto& row : rows)
        if (old_representative(row.second, rows, live, cohorts))
            expected_pins.push_back(&row.second);
    require(pins == expected_pins, "oldest dormant representative order differs");
    active->representatives += pins.size();
    if (!active->reference && active->outer_rows == 0 && !origins.empty()) {
        active->matrix = true;
        outer_truth_table(pin, cycle, interval, script_open);
        inner_truth_table(pin, origins, cycle, interval, script_open);
        active->matrix = false;
    }
}
} // namespace askable_fixture

// The existing observer exposes the real local predicate without replacing it.
// In the reference arm only, its callers use the independent old result.
// Both standard-algorithm wrappers are observations, including on the old TU.
#define PINEFORGE_RETIREMENT_ASKABLE_SWEEP() ::askable_fixture::next_sweep()
#define PINEFORGE_RETIREMENT_OBSERVE(rows, cycle, askable, roots, candidates, first) \
    ::askable_fixture::observe(rows, cycle, askable, origin_pins, live_handles_, \
        cohorts_by_id_.held_rows(), interval, last_broker_open_ms_, pinned_by_itself)
#define pinned_by_itself(value) \
    ::askable_fixture::select_pin(value, [&] { return pinned_by_itself(value); }, cycle, interval, \
        last_broker_open_ms_, placement_, live_handles_, cohorts_by_id_.held_rows())
#define sort(first, ...) sort(::askable_fixture::sorted_first(first), __VA_ARGS__)
#define binary_search(first, ...) binary_search(::askable_fixture::searched_first(first), __VA_ARGS__)
#include "../src/source/pine_adapter.cpp"
#undef binary_search
#undef sort
#undef pinned_by_itself
#undef PINEFORGE_RETIREMENT_OBSERVE
#undef PINEFORGE_RETIREMENT_ASKABLE_SWEEP

namespace askable_fixture {
using Adapter = ps::PineExecutionAdapter;
template<class Tag, auto Member>
struct Access { friend auto access(Tag) { return Member; } };
struct Placements { friend auto access(Placements); };
struct Scratch { friend auto access(Scratch); };
struct PairHold { friend auto access(PairHold); };
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Scratch, &Adapter::retired_row_scratch_>;
template struct Access<PairHold, &Adapter::hold_reversal_pair_brackets>;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

struct Bytes {
    std::string value;
    template<class T> void scalar(const T& v) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "scalar fields only");
        value.append(reinterpret_cast<const char*>(&v), sizeof v);
    }
    void text(const std::string& v) {
        scalar(static_cast<std::uint64_t>(v.size()));
        value.append(v);
    }
};

void trade_stats(Bytes& out, const pf_trade_stats_t& s) {
#define FIELD(name) out.scalar(s.name)
    FIELD(num_trades); FIELD(num_wins); FIELD(num_losses); FIELD(num_even);
    FIELD(percent_profitable); FIELD(net_profit); FIELD(net_profit_pct);
    FIELD(gross_profit); FIELD(gross_profit_pct); FIELD(gross_loss); FIELD(gross_loss_pct);
    FIELD(profit_factor); FIELD(avg_trade); FIELD(avg_trade_pct);
    FIELD(avg_win); FIELD(avg_win_pct); FIELD(avg_loss); FIELD(avg_loss_pct);
    FIELD(ratio_avg_win_avg_loss); FIELD(largest_win); FIELD(largest_win_pct);
    FIELD(largest_loss); FIELD(largest_loss_pct); FIELD(commission_paid); FIELD(expectancy);
    FIELD(max_consecutive_wins); FIELD(max_consecutive_losses);
    FIELD(avg_bars_in_trade); FIELD(avg_bars_in_wins); FIELD(avg_bars_in_losses);
#undef FIELD
}

void equity_stats(Bytes& out, const pf_equity_stats_t& s) {
#define FIELD(name) out.scalar(s.name)
    FIELD(max_equity_drawdown); FIELD(max_equity_drawdown_pct);
    FIELD(max_equity_runup); FIELD(max_equity_runup_pct);
    FIELD(buy_hold_return); FIELD(buy_hold_return_pct);
    FIELD(sharpe_monthly); FIELD(sortino_monthly); FIELD(sharpe_bar); FIELD(sortino_bar);
    FIELD(cagr); FIELD(calmar); FIELD(recovery_factor); FIELD(time_in_market_pct); FIELD(open_pl);
#undef FIELD
}

// Pointer-free bytes of every report field and array, including NaN payloads
// and every recorded broker hash. No object padding or pointer address is compared.
void report_bytes(Bytes& out, const ReportC& r) {
#define FIELD(name) out.scalar(r.name)
    FIELD(total_trades); FIELD(trades_len); FIELD(net_profit);
    FIELD(input_bars_processed); FIELD(script_bars_processed);
    FIELD(security_feeds_total); FIELD(security_eval_complete_total);
    FIELD(security_eval_partial_total); FIELD(magnifier_sub_bars_total);
    FIELD(magnifier_sample_ticks_total); FIELD(input_tf_seconds); FIELD(script_tf_seconds);
    FIELD(script_tf_ratio); FIELD(needs_aggregation); FIELD(bar_magnifier_enabled);
    FIELD(security_diag_len); FIELD(trace_len); FIELD(trace_names_len);
    FIELD(equity_curve_len); FIELD(broker_state_hash_len);
#undef FIELD
    trade_stats(out, r.metrics.all); trade_stats(out, r.metrics.longs);
    trade_stats(out, r.metrics.shorts); equity_stats(out, r.metrics.equity);
    for (int i = 0; i < r.trades_len; ++i) {
        const auto& t = r.trades[i];
#define FIELD(name) out.scalar(t.name)
        FIELD(entry_time); FIELD(exit_time); FIELD(entry_price); FIELD(exit_price);
        FIELD(pnl); FIELD(pnl_pct); FIELD(is_long); FIELD(max_runup); FIELD(max_drawdown);
        FIELD(qty); FIELD(commission); FIELD(entry_bar_index); FIELD(exit_bar_index); FIELD(open_at_end);
#undef FIELD
    }
    for (int i = 0; i < r.security_diag_len; ++i) {
        const auto& d = r.security_diag[i];
        out.scalar(d.sec_id); out.scalar(d.feed_count);
        out.scalar(d.eval_complete_count); out.scalar(d.eval_partial_count);
    }
    for (int i = 0; i < r.trace_len; ++i) {
        const auto& t = r.trace[i];
        out.scalar(t.timestamp); out.scalar(t.bar_index);
        out.scalar(t.name_id); out.scalar(t.value);
    }
    for (int i = 0; i < r.trace_names_len; ++i) out.text(r.trace_names[i]);
    for (std::int64_t i = 0; i < r.equity_curve_len; ++i) {
        const auto& e = r.equity_curve[i];
        out.scalar(e.time_ms); out.scalar(e.equity); out.scalar(e.open_profit);
    }
    for (std::int64_t i = 0; i < r.broker_state_hash_len; ++i)
        out.scalar(r.broker_state_hash[i]);
}

struct Outcome {
    Work work;
    std::vector<Origins> rows, released;
    std::string report;
    std::size_t bracket_rows = 0;
    int trades = 0;
};

class Host final : public ps::PineStrategyHost {
public:
    explicit Host(bool quiet, Work& work) : quiet_(quiet), work_(work) {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 4.0;
        config.pyramiding = 2;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = quiet;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_broker_state_hash_recording(true);
        auto& origins = (adapter_.*access(Scratch{})).askable_origins;
        // Give even an empty range a unique backing address. Capacity is
        // scratch only; no placement row, order input or durable state is seeded.
        origins.reserve(64);
        work_.origins = &origins;
    }

    void on_source_bar(const Bar&) override {
        capture();
        const int bar = pine_bar_index();
        if (quiet_) {
            if (bar == 0) {
                require(live_position_size() == 0.0, "quiet entry must start flat");
                strategy_entry("entry", true);
            }
            // The close fill does not recalculate bar 0. Its filled position
            // is available to the next bar's source callback.
            if (bar == 1) {
                require(live_position_size() > 0.0, "quiet bracket needs a filled entry");
                strategy_exit("bracket", "", 120.0, 80.0);
            }
            return;
        }
        if (bar == 0 || bar == 14) strategy_entry("entry", true);
        if (bar >= 1 && bar <= 8) {
            require(live_position_size() > 0.0, "named brackets need the first filled entry");
            if (bar % 2 == 0) strategy_cancel("bracket");
            strategy_exit("bracket", "entry", 120.0 + bar, 80.0 - bar,
                          kNa, kNa, kNa, 100.0, {}, 4.0);
            if (bar == 3 || bar == 7) {
                // Use the real pair-hold transition and its current frame,
                // as the neighbouring lifecycle-retention fixtures do.
                (adapter_.*access(PairHold{}))("entry");
            }
        }
        if (bar == 10 || bar == 20) strategy_close_all();
        if (bar >= 15 && bar <= 18) {
            require(live_position_size() > 0.0, "global brackets need the second filled entry");
            strategy_exit("global", "", 125.0 + bar, 75.0 - bar);
        }
    }

    void capture() {
        Origins kept;
        for (const auto& row : adapter_.*access(Placements{})) kept.push_back(row.first);
        rows_.push_back(std::move(kept));
        const auto& doomed = (adapter_.*access(Scratch{})).doomed;
        released_.push_back(doomed);
        work_.retained += rows_.back().size();
        work_.released += doomed.size();
    }

    Outcome finish() {
        if (!last_error().empty()) throw std::runtime_error("strategy run failed: " + last_error());
        capture();
        Outcome result;
        result.rows = rows_;
        result.released = released_;
        for (const auto& row : adapter_.*access(Placements{}))
            if (exit_family(row.second.family) && row.second.legs.target().incarnation != 0)
                ++result.bracket_rows;
        struct ReportOwner {
            ReportC value{};
            ~ReportOwner() { BacktestEngine::free_report(&value); }
        } report;
        fill_report(&report.value);
        require(report.value.broker_state_hash_len > 0, "broker hash timeline was not recorded");
        Bytes bytes;
        report_bytes(bytes, report.value);
        // ReportC omits these native trade identity fields.
        for (int i = 0; i < trade_count(); ++i) {
            const auto t = get_trade(i);
            bytes.text(t.entry_id); bytes.text(t.exit_id);
            bytes.text(t.entry_comment); bytes.text(t.exit_comment);
            bytes.scalar(t.entry_incarnation); bytes.scalar(t.exit_from_bracket);
            bytes.scalar(t.close_cause);
        }
        result.report = std::move(bytes.value);
        result.trades = trade_count();
        result.work = work_;
        result.work.origins = nullptr;
        return result;
    }

    void hash_host_extension(BrokerStateHashSink& sink) const override {
        ps::PineStrategyHost::hash_host_extension(sink);
        sink.b(quiet_);
    }

private:
    const bool quiet_;
    Work& work_; // Observation only; not read by strategy decisions or hashing.
    std::vector<Origins> rows_, released_;
};

Outcome run(bool quiet, bool reference) {
    require(active == nullptr, "observation scopes must not nest");
    Work work;
    work.reference = reference;
    struct Scope {
        const bool previous = ps::detail::retain_retired_rows();
        explicit Scope(Work& work) {
            ps::detail::set_retain_retired_rows(false);
            active = &work;
        }
        ~Scope() {
            active = nullptr;
            ps::detail::set_retain_retired_rows(previous);
        }
    } scope(work);
    Host host(quiet, work);
    std::vector<Bar> bars;
    for (int i = 0; i < (quiet ? 2 : 24); ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "",
             false, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

void quiet_brackets_need_no_origin_work() {
    const auto actual = run(true, false);
    std::printf("quiet calls=%llu builds=%llu searches=%llu bracket_rows=%zu\n",
                static_cast<unsigned long long>(actual.work.calls),
                static_cast<unsigned long long>(actual.work.builds),
                static_cast<unsigned long long>(actual.work.searches), actual.bracket_rows);
    require(actual.bracket_rows >= 2, "quiet script did not create real bracket legs");
    require(actual.work.calls > 0, "quiet script did not enter the retirement predicate");
    require(actual.work.builds == 0, "quiet bracket sweep built askable origins");
    require(actual.work.searches == 0, "quiet bracket sweep searched askable origins");
}

void predicates_and_reports_match_eager_reference() {
    const auto actual = run(false, false);
    const auto reference = run(false, true);
    std::printf("predicate sweeps=%llu builds=%llu searches=%llu retained=%llu released=%llu\n",
                static_cast<unsigned long long>(actual.work.sweeps),
                static_cast<unsigned long long>(actual.work.builds),
                static_cast<unsigned long long>(actual.work.searches),
                static_cast<unsigned long long>(actual.work.retained),
                static_cast<unsigned long long>(actual.work.released));
    require(actual.work.outer_rows == 8 && actual.work.inner_rows == 8,
            "production predicate truth tables were not reached");
    require(actual.work.sweeps > 1 && actual.work.builds > 0 && actual.work.searches > 0,
            "script did not exercise origin construction and membership");
    require(actual.work.shared_build_sweeps > 0
            || (actual.work.sweep_builds == 1 && actual.work.sweep_searches > 1),
            "multiple origin queries did not share a single build");
    require(actual.work.retained > 0 && actual.work.released > 0,
            "script must both retain and release real rows");
    require(actual.work.representatives > 0, "script did not exercise dormant origin pins");
    require(actual.trades > 0, "report comparison must contain real trades");
    require(actual.rows == reference.rows, "retained placement order differs from eager reference");
    require(actual.released == reference.released, "released placement order differs from eager reference");
    require(actual.report == reference.report, "report fields or broker hash bytes differ");
}
} // namespace askable_fixture

int main(int argc, char** argv) {
    try {
        const bool all = argc == 1;
        const bool quiet = argc == 2 && std::strcmp(argv[1], "quiet") == 0;
        const bool predicate = argc == 2 && std::strcmp(argv[1], "predicate") == 0;
        askable_fixture::require(all || quiet || predicate, "expected quiet or predicate selector");
        if (all || quiet) askable_fixture::quiet_brackets_need_no_origin_work();
        if (all || predicate) askable_fixture::predicates_and_reports_match_eager_reference();
        std::puts("test_pine_adapter_askable_lazy: passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_pine_adapter_askable_lazy: %s\n", error.what());
        return 1;
    }
}
