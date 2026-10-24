// Measurement, never a growth oracle. Public orders mirror both Pine fixtures
// in fixtures/k1_dormant_reissue through the real source host.
// See REPORT-k1g.md: an oversized market reversal is close-only on this tree;
// a resting limit reversal supplies a tape-independent pair hold instead.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_placement_retention.hpp"

#include <sys/resource.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace ps = pineforge::source;

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
struct PlacementTag {
    using type = ps::PlacementTable ps::PineExecutionAdapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag, &ps::PineExecutionAdapter::placement_>;

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr double kCapital = 1000000000.0;
constexpr double kTick = 0.01;
constexpr std::int64_t kStart = 1736121600000LL;
constexpr std::int64_t kMinute = 60000LL;

enum class Pattern { Untouched, TouchedAll, TouchedHalf };

const char* name(Pattern pattern) {
    switch (pattern) {
    case Pattern::Untouched: return "untouched";
    case Pattern::TouchedAll: return "touched_armed_1";
    case Pattern::TouchedHalf: return "touched_armed_0.5";
    }
    return "invalid";
}

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "64-bit double required");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

std::uint64_t peak_rss_kib() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("K1G CONTROL FAILED: getrusage");
#if defined(__APPLE__)
    // Darwin reports bytes; Linux reports KiB.
    return static_cast<std::uint64_t>(usage.ru_maxrss) / 1024U;
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#endif
}

struct Sample {
    int bar = -1;
    std::size_t placements = 0;
    std::size_t exit_rows = 0;
    std::size_t lifecycles = 0;
    std::size_t dormant_rows = 0;
    std::size_t retired_dormant = 0;
    std::size_t predecessors = 0;
    std::size_t native_pending = 0;
    std::size_t pending_live = 0;
    std::size_t pending_dormant = 0;
    int pending_api = 0;
};

struct Outcome {
    std::vector<Sample> checkpoints;
    std::vector<Trade> trades;
    double rows_per_bar = 0.0;
    double dead_dormant_per_bar = 0.0;
};

struct RetentionSwitch {
    explicit RetentionSwitch(bool retain) : previous(ps::detail::retain_retired_rows()) {
        ps::detail::set_retain_retired_rows(retain);
    }
    ~RetentionSwitch() { ps::detail::set_retain_retired_rows(previous); }
    bool previous;
};

class Host final : public ps::PineStrategyHost {
public:
    Host(int bars, Pattern pattern, bool retain)
        : bars_(bars), pattern_(pattern), retain_(retain) {
        ps::PineStrategyConfig config;
        config.initial_capital = kCapital;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(kTick);
        set_margin_call_enabled(true);
        // Recording a hash history would add an unrelated per-bar allocation.
        set_broker_state_hash_recording(false);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        require(i == callbacks_, "one callback per consecutive minute");
        ++callbacks_;
        require(bar.timestamp == kStart + static_cast<std::int64_t>(i) * kMinute,
                "1m timestamps");
        require(bar.low > kTick && bar.high < kCapital / 4.0,
                "ETH envelope: limit cannot fill, short cannot be margined");
        if (i >= 1 && bar.high >= previous_close_ + 2.0 * kTick) ++raw_near_hits_;
        if (pattern_ != Pattern::Untouched && i >= 3 && i % 2 == 1)
            check_prior_touch(bar);
        observe("before", sampled(i));
        if (i == 0) strategy_entry("S", false, kNa, kNa, 1.0);
        if (i >= 1 && live_position_size() < 0.0) {
            const bool near = pattern_ != Pattern::Untouched && i % 2 == 0;
            const double stop = near ? bar.close + 2.0 * kTick
                : kCapital + static_cast<double>(i) * 3.0 * kTick;
            require(std::isfinite(stop) && stop > bar.close, "short stop above close");
            if (!near) require(stop > bar.high, "far seed is untouched");
            require(i == 1 || bits(stop) != last_stop_bits_, "stop changes each bar");
            last_stop_bits_ = bits(stop);
            strategy_exit("X", "S", kNa, stop, kNa, kNa, kNa, 100.0, {}, 0.5);
            // The actual close lowering selects hold_reversal_pair_brackets.
            // A resting one-tick LIMIT avoids the unaffordable market
            // reversal's close-only lowering; no private lifecycle is called.
            strategy_entry("L", true, kTick, kNa, kCapital);
            strategy_close("S");
            ++exit_calls_;
        }
        observe("after_commands", sampled(i));
        previous_close_ = bar.close;
        // Fresh explicit exits drain after this callback. The next "before"
        // observation sees that drain and the real bar-open retirement sweep.
    }

    Outcome finish() {
        require(last_error().empty(), "run completed without error");
        require(callbacks_ == bars_, "all N bars calculated");
        require(exit_calls_ == bars_ - 1, "explicit exit issued on every held bar");
        check_trades();
        const Sample last = observe("post_drain_end", true);
        require(midpoint_seen_ && checkpoints_.size() == 10,
                "all ten measurement checkpoints exist");
        // Both endpoints are after commands, BEFORE the deferred drain.
        // Do not mix that phase with post_drain_end's fresh explicit leg.
        const Sample& end = checkpoints_.back();
        const int elapsed = end.bar - midpoint_.bar;
        const double rows_per_bar = (static_cast<double>(end.placements)
            - static_cast<double>(midpoint_.placements)) / elapsed;
        const double dead_per_bar = (static_cast<double>(end.retired_dormant)
            - static_cast<double>(midpoint_.retired_dormant)) / elapsed;
        const std::uint64_t rss = peak_rss_kib();
        std::printf(
            "K1G END pattern=%s retention=%s N=%d callbacks=%d exit_calls=%d position=%.17g "
            "placement=%zu peak_placement=%zu snapshot_bytes=%zu "
            "slope_rows_per_bar=%.9g slope_X_dead_dormant_per_bar=%.9g "
            "placement_bytes_per_bar=%.9g ru_maxrss_kib=%llu "
            "peak_rss_delta_bytes_per_bar=%.9g slope_start_bar=%d slope_end_bar=%d "
            "armed_opportunities=%d armed_touches=%d armed_touch_fraction=%.9g "
            "calendar_touch_fraction=%.9g raw_near_hits=%d raw_near_fraction=%.9g "
            "controls=PASS measurement_only=1\n",
            name(pattern_), mode(), bars_, callbacks_, exit_calls_,
            live_position_size(), last.placements,
            peak_placements_, sizeof(ps::PlacementSnapshot),
            rows_per_bar, dead_per_bar,
            rows_per_bar * static_cast<double>(sizeof(ps::PlacementSnapshot)),
            static_cast<unsigned long long>(rss),
            (static_cast<double>(rss) - static_cast<double>(midpoint_rss_)) * 1024.0 / elapsed,
            midpoint_.bar, end.bar, armed_opportunities_, armed_touches_,
            armed_opportunities_ ? static_cast<double>(armed_touches_) / armed_opportunities_ : 0.0,
            static_cast<double>(armed_touches_) / bars_, raw_near_hits_,
            static_cast<double>(raw_near_hits_) / (bars_ - 1));
        std::fflush(stdout);
        Outcome outcome;
        outcome.checkpoints = checkpoints_;
        outcome.rows_per_bar = rows_per_bar;
        outcome.dead_dormant_per_bar = dead_per_bar;
        for (int i = 0; i < report_trade_count(); ++i)
            outcome.trades.push_back(get_report_trade(i));
        return outcome;
    }

private:
    const char* mode() const { return retain_ ? "RETAIN" : "ERASE"; }

    void require(bool holds, const char* control) const {
        if (!holds) {
            std::fprintf(stderr,
                         "K1G CONTROL FAILED pattern=%s retention=%s N=%d bar=%d: %s error=\"%s\"\n",
                         name(pattern_), mode(), bars_, pine_bar_index(),
                         control, last_error().c_str());
            throw std::runtime_error(std::string("K1G CONTROL FAILED: ") + control);
        }
    }

    bool sampled(int i) const {
        return i < 5 || (i + 1) % (bars_ / 10) == 0;
    }

    void check_trades() const {
        for (int i = 0; i < trade_count(); ++i) {
            const Trade trade = get_trade(i);
            require(trade.exit_id != ps::kMarginCallLabel, "no margin call");
            // Historical range-end marks do not close the physical position.
            require(trade.open_at_end, "no bracket, reversal or close filled");
        }
    }

    bool is_x(const ps::PlacementSnapshot& row) const {
        return row.family == ps::PineOrderFamily::ExitStop
            && row.source_id == "X" && row.from_entry == "S";
    }

    void check_leg(const ps::PlacementSnapshot& row) const {
        require(row.cancellation.cause != ps::PineCancellationCause::Explicit,
                "X is never cancelled");
        if (row.legs.target().incarnation == 0) return;
        require(row.bracket_origin.incarnation != 0 && row.requested_qty == 0.5,
                "explicit half-quantity leg belongs to the opened origin");
        require(!row.legs.retired(exit_legs::Leg::Stop), "stop was not withdrawn");
    }

    void check_prior_touch(const Bar& bar) {
        const auto& placements = adapter_.*access(PlacementTag{});
        const auto found = placements.find(armed_incarnation_);
        require(found != placements.end(), "armed leg survives to its touch bar");
        const auto& row = found->second;
        check_leg(row);
        require(row.legs.dormant(), "near stop is dormant on its evaluation bar");
        bool working = false;
        for (const auto& request : native_working_requests())
            if (request.definition->handle.incarnation == armed_incarnation_) working = true;
        const bool touched = bar.high >= row.exit_levels.stop;
        last_armed_touched_ = touched;
        ++armed_opportunities_;
        if (touched) {
            ++armed_touches_;
            require(!working && row.cancellation.cause == ps::PineCancellationCause::Admission,
                    "touched dormant native stop retired without closing S");
        } else {
            require(working, "untouched armed stop remains native pending");
        }
    }

    Sample observe(const char* phase, bool print) {
        const int i = pine_bar_index();
        require(ps::detail::retain_retired_rows() == retain_, "requested retention mode");
        if (i >= 1) {
            require(live_position_size() == -1.0, "one short remains open");
            require(adapter_.source_unclosed_qty_for("S") == 1.0,
                    "the S origin stays open");
        }
        check_trades();
        const auto working = native_working_requests();
        const auto is_working = [&](std::uint64_t incarnation) {
            for (const auto& request : working)
                if (request.definition->handle.incarnation == incarnation) return true;
            return false;
        };
        const auto& placements = adapter_.*access(PlacementTag{});
        Sample sample;
        sample.bar = i;
        sample.placements = placements.size();
        sample.native_pending = working.size();
        sample.pending_api = pending_order_count();
        bool working_dormant_x = false;
        bool fresh_x = false;
        const bool expect_fresh = pattern_ != Pattern::Untouched && last_armed_touched_
            && ((i >= 4 && i % 2 == 0 && std::strcmp(phase, "before") == 0)
                || std::strcmp(phase, "post_drain_end") == 0);
        for (const auto& request : working) {
            const auto found = placements.find(request.definition->handle.incarnation);
            require(found != placements.end(), "every pending request has a placement row");
            const auto& row = found->second;
            if (row.legs.dormant()) ++sample.pending_dormant;
            else ++sample.pending_live;
            if (!is_x(row)) continue;
            check_leg(row);
            if (expect_fresh) {
                require(row.projection_predecessor == 0 && !row.legs.dormant(),
                        "after a touch the far seed is fresh with no projection predecessor");
                fresh_x = true;
            }
            working_dormant_x = working_dormant_x || row.legs.dormant();
            if (pattern_ != Pattern::Untouched && i >= 2 && i % 2 == 0
                && std::strcmp(phase, "after_commands") == 0) {
                require(row.legs.dormant(), "even-bar near replacement is pair held");
                armed_incarnation_ = request.definition->handle.incarnation;
            }
        }
        if (pattern_ == Pattern::Untouched && i >= 3)
            require(working_dormant_x, "untouched exit is dormant after bar 3");
        if (expect_fresh) require(fresh_x, "fresh far seed drained after the touch callback");
        if (pattern_ != Pattern::Untouched && i >= 2 && i % 2 == 0
            && std::strcmp(phase, "after_commands") == 0)
            require(working_dormant_x, "a working dormant near stop was armed");
        if (sample.placements > peak_placements_) peak_placements_ = sample.placements;
        // Controls above visit only the working book / last armed handle.
        // RETAIN must not turn the observer into an extra whole-table walk
        // on every bar. Scan historical rows only at printed checkpoints.
        if (!print) return sample;
        for (const auto& placement : placements) {
            const auto& row = placement.second;
            const bool live = is_working(placement.first);
            if (!is_x(row)) continue;
            ++sample.exit_rows;
            if (row.projection_predecessor != 0) ++sample.predecessors;
            check_leg(row);
            if (row.legs.target().incarnation == 0) continue;
            ++sample.lifecycles;
            if (row.legs.dormant()) {
                ++sample.dormant_rows;
                if (!live) ++sample.retired_dormant;
            }
        }
        if (std::strcmp(phase, "after_commands") == 0 && (i + 1) % (bars_ / 10) == 0) {
            checkpoints_.push_back(sample);
            if (i + 1 == bars_ / 2) {
                midpoint_ = sample;
                midpoint_rss_ = peak_rss_kib();
                midpoint_seen_ = true;
            }
        }
        std::printf(
            "K1G pattern=%s retention=%s N=%d bar=%d completed=%d phase=%s placement=%zu "
            "exit_rows=%zu lifecycle_rows=%zu dormant_rows=%zu retired_dormant=%zu "
            "predecessor_rows=%zu pending_api=%d native_pending=%zu "
            "pending_live=%zu pending_dormant=%zu snapshot_payload_bytes=%zu "
            "ru_maxrss_kib=%llu\n",
            name(pattern_), mode(), bars_, i, i + 1, phase, sample.placements, sample.exit_rows,
            sample.lifecycles, sample.dormant_rows, sample.retired_dormant,
            sample.predecessors, sample.pending_api, sample.native_pending,
            sample.pending_live, sample.pending_dormant,
            sample.placements * sizeof(ps::PlacementSnapshot),
            static_cast<unsigned long long>(peak_rss_kib()));
        std::size_t printed = 0;
        for (const auto& placement : placements) {
            const auto& row = placement.second;
            if (!is_x(row)) continue;
            // Print the first two historical X rows plus all working / last
            // armed legs. Aggregates above count EVERY retained X row.
            const bool live = is_working(placement.first);
            if (printed >= 2 && !live && placement.first != armed_incarnation_) continue;
            ++printed;
            std::printf(
                "K1G LEG pattern=%s retention=%s N=%d bar=%d phase=%s inc=%llu native_working=%d "
                "target=%llu origin=%llu dormant=%d stop_retired=%d "
                "hold_bar=%lld excluded_bar=%lld predecessor=%llu qty=%.17g "
                "stop=%.17g stop_bits=%016llx cancellation=%d\n",
                name(pattern_), mode(), bars_, i, phase,
                static_cast<unsigned long long>(placement.first), live ? 1 : 0,
                static_cast<unsigned long long>(row.legs.target().incarnation),
                static_cast<unsigned long long>(row.bracket_origin.incarnation),
                row.legs.dormant() ? 1 : 0,
                row.legs.retired(exit_legs::Leg::Stop) ? 1 : 0,
                static_cast<long long>(row.legs.hold_bar()),
                static_cast<long long>(row.legs.excluded_bar()),
                static_cast<unsigned long long>(row.projection_predecessor),
                row.requested_qty, row.exit_levels.stop,
                static_cast<unsigned long long>(bits(row.exit_levels.stop)),
                static_cast<int>(row.cancellation.cause));
        }
        std::printf("K1G LEG_COVERAGE printed=%zu total_X=%zu all_X_counted=1\n",
                    printed, sample.exit_rows);
        std::fflush(stdout);
        return sample;
    }

    // Observation bookkeeping only; it never selects or changes an order.
    const int bars_;
    const Pattern pattern_;
    const bool retain_;
    int callbacks_ = 0;
    int exit_calls_ = 0;
    std::uint64_t last_stop_bits_ = 0;
    std::size_t peak_placements_ = 0;
    std::uint64_t armed_incarnation_ = 0;
    double previous_close_ = 0.0;
    int armed_opportunities_ = 0;
    int armed_touches_ = 0;
    bool last_armed_touched_ = false;
    int raw_near_hits_ = 0;
    bool midpoint_seen_ = false;
    Sample midpoint_{};
    std::uint64_t midpoint_rss_ = 0;
    std::vector<Sample> checkpoints_;
};

Outcome run(int count, Pattern pattern, bool retain, const std::vector<Bar>& bars) {
    RetentionSwitch switch_for_run(retain);
    Host host(count, pattern, retain);
    host.run(bars.data(), count, "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

bool same_number(double lhs, double rhs) {
    return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
}

const char* different_field(const Trade& lhs, const Trade& rhs) {
#define K1G_FIELD(field) if (lhs.field != rhs.field) return #field
#define K1G_NUMBER(field) if (!same_number(lhs.field, rhs.field)) return #field
    K1G_FIELD(entry_time);
    K1G_FIELD(exit_time);
    K1G_NUMBER(entry_price);
    K1G_NUMBER(exit_price);
    K1G_NUMBER(qty);
    K1G_NUMBER(pnl);
    K1G_NUMBER(pnl_pct);
    K1G_FIELD(is_long);
    K1G_FIELD(entry_bar_index);
    K1G_FIELD(exit_bar_index);
    K1G_FIELD(entry_id);
    K1G_FIELD(entry_comment);
    K1G_FIELD(exit_comment);
    K1G_FIELD(exit_id);
    K1G_FIELD(exit_from_bracket);
    K1G_NUMBER(max_runup);
    K1G_NUMBER(max_drawdown);
    K1G_NUMBER(commission);
    K1G_FIELD(entry_incarnation);
    K1G_FIELD(open_at_end);
    K1G_FIELD(close_cause);
#undef K1G_NUMBER
#undef K1G_FIELD
    return nullptr;
}

void measure(int count, Pattern pattern) {
    // Close is unchanged across an even arm / odd evaluation pair.
    // Wide highs reach every armed stop; the half tape alternates wide/narrow
    // highs on successive eligible odd bars. These are synthetic controls;
    // neither Pine fixture modifies or supplies the real ETH tape.
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double close = 2000.0 + static_cast<double>((i / 2) % 101) * kTick;
        const bool wide = pattern != Pattern::TouchedHalf || (i / 2) % 2 == 1;
        bars.push_back({close, close + (wide ? 1.0 : kTick), close - 1.0, close, 1.0,
                       kStart + static_cast<std::int64_t>(i) * kMinute});
    }
    // A failed ERASE control throws before the RETAIN run is started.
    const Outcome erased = run(count, pattern, false, bars);
    const Outcome retained = run(count, pattern, true, bars);
    bool equal = erased.trades.size() == retained.trades.size();
    const char* field = equal ? nullptr : "report_trade_count";
    std::size_t first = 0;
    while (equal && first < erased.trades.size()) {
        field = different_field(erased.trades[first], retained.trades[first]);
        if (field) equal = false;
        else ++first;
    }
    // Equality is printed evidence, never a literal trade roster or a
    // growth assertion. Include range-end rows, not just closed trades.
    for (std::size_t i = 0; i < erased.checkpoints.size(); ++i) {
        const auto& a = erased.checkpoints[i];
        const auto& b = retained.checkpoints[i];
        std::printf(
            "K1G COMPARE pattern=%s N=%d bar=%d ERASE_placement=%zu RETAIN_placement=%zu "
            "erase_vs_retain_trade_equal=%d ERASE_X_dead_dormant=%zu "
            "RETAIN_X_dead_dormant=%zu equality_scope=complete_final_report\n",
            name(pattern), count, a.bar, a.placements, b.placements, equal ? 1 : 0,
            a.retired_dormant, b.retired_dormant);
    }
    std::printf(
        "K1G COMPARE_END pattern=%s N=%d ERASE_rows_per_bar=%.9g RETAIN_rows_per_bar=%.9g "
        "erase_vs_retain_trade_equal=%d ERASE_X_dead_dormant_per_bar=%.9g "
        "RETAIN_X_dead_dormant_per_bar=%.9g ERASE_report_trades=%zu RETAIN_report_trades=%zu "
        "first_difference=%zu field=%s growth_assertions=none\n",
        name(pattern), count, erased.rows_per_bar, retained.rows_per_bar, equal ? 1 : 0,
        erased.dead_dormant_per_bar, retained.dead_dormant_per_bar,
        erased.trades.size(), retained.trades.size(), first, field ? field : "none");
    std::fflush(stdout);
}
} // namespace

int main() {
    try {
        std::printf(
            "K1G pattern=resting_limit_pair declined_market_reversal=0 "
            "touched_cadence=seed_odd_arm_even armed_touch_rates=1,0.5 "
            "snapshot_bytes=%zu rss_scope=process_high_water "
            "hash_history=off growth_assertions=none\n", sizeof(ps::PlacementSnapshot));
        for (const int count : {10000, 100000}) {
            measure(count, Pattern::Untouched);
            measure(count, Pattern::TouchedAll);
            measure(count, Pattern::TouchedHalf);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "K1G stopped: %s\n", error.what());
        return 1;
    }
}
