#pragma once

// Derived from k1g's dormant-reissue measurement and the frozen K1 public
// tape. Unlike an untouched pair-held request, each held stop is TOUCHED:
// its native request retires without withdrawing its dormant lifecycle.
// Private access observes rows only; every order/lifecycle transition is real.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_placement_retention.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace dormant_retirement {
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
constexpr std::int64_t kStart = 1736121600000LL;
constexpr std::int64_t kMinute = 60000LL;

// One origin representative, two distinct-stop witnesses, and at most one
// new explicit exit between sweeps. Y's legacy first witness is the origin
// representative A on THIS tape (150 -> 120), not an additional row.
constexpr std::size_t kOriginRepresentatives = 1;
constexpr std::size_t kStopWitnesses = 2;
constexpr std::size_t kNewExitsPerBar = 1;
constexpr std::size_t kOriginRowBound =
    kOriginRepresentatives + kStopWitnesses + kNewExitsPerBar;
// Open S, origin-free Y, and current/previous script-bar L commands (K5).
// The paired close is a source shadow, not a native placement.
constexpr std::size_t kOtherRows = 1 + 1 + 2;
constexpr std::size_t kPlacementRowBound = kOriginRowBound + kOtherRows;

inline void require(bool holds, const char* message) {
    if (!holds) throw std::runtime_error(message);
}

inline std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "64-bit double required");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline std::uint64_t peak_rss_kib() {
    struct rusage usage {};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss) / 1024U;
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#endif
}

inline double stop_for(int issue) { return issue % 2 == 0 ? 120.0 : 125.0; }

struct Outcome {
    std::vector<Trade> trades;
    std::vector<std::uint64_t> public_reads;
    std::size_t peak_rows = 0, final_rows = 0;
    std::size_t peak_origin_rows = 0, final_origin_rows = 0;
    int callbacks = 0, exit_calls = 0, settled_exits = 0;
    bool retain = false, latest_trap_seen = false, different_witness_seen = false;
    double position = 0.0, equity = 0.0;
    std::uint64_t rss_kib = 0;
};

class Host final : public ps::PineStrategyHost {
public:
    // margin=false: issue every bar from b2 through the end.
    // margin=true: hold/retire `held` exits, issue live C, wait, then margin.
    Host(int count, int held, bool margin, bool retain)
        : count_(count), held_(held), margin_(margin), retain_(retain) {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(true);
        set_broker_state_hash_recording(false);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        check(i == out_.callbacks, "one callback per consecutive minute");
        ++out_.callbacks;
        check(bar.timestamp == kStart + static_cast<std::int64_t>(i) * kMinute,
              "synthetic minute timestamp");
        observe();
        const int live_c_bar = held_ + 2;
        const int margin_bar = held_ + 4;
        if (i >= 1 && (!margin_ || i < margin_bar))
            check(live_position_size() == -40.0
                  && adapter_.source_unclosed_qty_for("S") == 40.0,
                  "one unchanged open S origin");
        if (i >= 3 && (!margin_ || i <= live_c_bar)) check_settled_exit(bar, i - 1);
        if (margin_ && i == margin_bar - 1) check_latest_trap();

        if (i == 0) strategy_entry("S", false, kNa, kNa, 40.0);
        if (i == 1)
            strategy_exit("X", "S", kNa, 150.0, kNa, kNa, kNa, 100.0, "Y");
        if (i >= 2 && (!margin_ || i < live_c_bar)) {
            const double stop = stop_for(i - 2);
            check(i == 2 || bits(stop) != previous_stop_, "stop changes every issue");
            previous_stop_ = bits(stop);
            strategy_exit("X", "S", kNa, stop, kNa, kNa, kNa, 100.0, "held", 20.0);
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
            ++out_.exit_calls;
        }
        if (margin_ && i == live_c_bar) {
            strategy_exit("X", "S", kNa, 120.0, kNa, kNa, kNa, 100.0, "C", 20.0);
            ++out_.exit_calls;
        }
        observe();
    }

    Outcome finish() {
        check(last_error().empty(), "run completed without error");
        check(out_.callbacks == count_, "all requested bars calculated");
        check(out_.exit_calls == (margin_ ? held_ + 1 : count_ - 2),
              "every requested exit issued");
        check(out_.settled_exits == (margin_ ? held_ : count_ - 3),
              "every reaching held exit retired before the next reissue");
        observe();
        out_.retain = ps::detail::retain_retired_rows();
        out_.position = live_position_size();
        out_.equity = current_equity();
        out_.rss_kib = peak_rss_kib();
        for (int i = 0; i < trade_count(); ++i) out_.trades.push_back(get_trade(i));
        if (!margin_) {
            check(out_.position == -40.0, "growth run holds the cycle open to the end");
            for (const auto& trade : out_.trades)
                check(trade.open_at_end, "growth run has no actual close or margin call");
        }
        return out_;
    }

    void hash_host_extension(BrokerStateHashSink& f) const override {
        ps::PineStrategyHost::hash_host_extension(f);
        f.i(count_); f.i(held_); f.b(margin_);
        // Retention mode and observation counters never select orders.
    }

private:
    void check(bool holds, const char* message) const {
        if (holds) return;
        std::fprintf(stderr, "DORMANT CONTROL FAILED N=%d bar=%d retain=%d: %s error=\"%s\"\n",
                     count_, pine_bar_index(), retain_ ? 1 : 0, message, last_error().c_str());
        throw std::runtime_error(message);
    }

    bool working(std::uint64_t incarnation) const {
        for (const auto& request : native_working_requests())
            if (request.definition->handle.incarnation == incarnation) return true;
        return false;
    }

    void check_settled_exit(const Bar& bar, int issue_bar) {
        const auto& rows = adapter_.*access(PlacementTag{});
        int exits = 0, reversals = 0;
        for (const auto row : rows) {
            const auto& value = row.second;
            if (value.placement_script_open_ms != kStart + issue_bar * kMinute) continue;
            if (value.family == ps::PineOrderFamily::Entry && value.source_id == "L") {
                check(value.reverse_to && value.opening && !value.signal_price_refused
                      && !working(row.first) && std::isfinite(value.sizing.frozen_units)
                      && value.sizing.frozen_units > 0.0 && std::isfinite(value.sizing.equity)
                      && value.sizing.frozen_units * bar.open
                          > value.sizing.equity + std::abs(value.sizing.equity) * 1e-12,
                      "public reversal declined at the adverse opening");
                ++reversals;
            }
            if (value.family != ps::PineOrderFamily::ExitStop
                || value.source_id != "X" || value.from_entry != "S"
                || value.bracket_origin.incarnation == 0) continue;
            check(value.legs.target().incarnation != 0 && value.legs.dormant()
                  && !value.legs.retired(exit_legs::Leg::Stop)
                  && value.cancellation.cause != ps::PineCancellationCause::Explicit
                  && !working(row.first) && value.projection_predecessor == 0,
                  "touched, native-retired, unwithdrawn dormant exit without a projection link");
            check(value.requested_qty == 20.0
                  && bits(value.exit_levels.stop) == bits(stop_for(issue_bar - 2))
                  && bar.open < value.exit_levels.stop && bar.high >= value.exit_levels.stop,
                  "the actual changed stop was touched without an opening gap");
            if (origin_ == 0) {
                origin_ = value.bracket_origin.incarnation;
                cycle_ = value.placement_cycle;
            }
            check(value.bracket_origin.incarnation == origin_ && value.placement_cycle == cycle_,
                  "all issues belong to one origin and cycle");
            ++exits;
        }
        check(exits == 1 && reversals == 1, "one real retired exit and declined reversal per issue");
        ++out_.settled_exits;
    }

    void check_latest_trap() {
        // A's stop equals live C's stop; C alone cannot supersede A. The
        // previous 125 row must remain a later-bar, different-bit witness.
        bool a = false, b = false, c = false;
        for (const auto row : adapter_.*access(PlacementTag{})) {
            const auto& value = row.second;
            if (value.family != ps::PineOrderFamily::ExitStop || value.source_id != "X"
                || value.from_entry != "S" || value.bracket_origin.incarnation != origin_)
                continue;
            const auto placed = value.placement_script_open_ms;
            if (placed == kStart + 2 * kMinute)
                a = value.legs.dormant() && !working(row.first) && value.exit_levels.stop == 120.0;
            if (placed == kStart + (held_ + 1) * kMinute)
                b = value.legs.dormant() && !working(row.first) && value.exit_levels.stop == 125.0;
            if (placed == kStart + (held_ + 2) * kMinute)
                c = !value.legs.dormant() && working(row.first) && value.exit_levels.stop == 120.0;
        }
        check(a && c, "dormant A and live equal-stop C reach the margin trap");
        if (retain_) check(b, "RETAIN contains the later different-stop B");
        // Let an ERASE mutation that lost B reach the actual margin call:
        // the trade comparison must diagnose its extra revived-A fill.
        out_.different_witness_seen = b;
        out_.latest_trap_seen = true;
    }

    void observe() {
        check(ps::detail::retain_retired_rows() == retain_, "requested retention mode");
        const auto& rows = adapter_.*access(PlacementTag{});
        out_.final_rows = rows.size();
        out_.final_origin_rows = 0;
        for (const auto row : rows) {
            const auto& value = row.second;
            if (value.family == ps::PineOrderFamily::ExitStop && value.source_id == "X"
                && value.from_entry == "S" && value.bracket_origin.incarnation != 0
                && value.legs.target().incarnation != 0) ++out_.final_origin_rows;
        }
        out_.peak_rows = std::max(out_.peak_rows, out_.final_rows);
        out_.peak_origin_rows = std::max(out_.peak_origin_rows, out_.final_origin_rows);
        // Only small correctness runs retain observation history.
        if (!margin_) return;
        BrokerStateHashSink f;
        f.d(live_position_size()); f.d(position_avg_price()); f.d(current_equity());
        f.i(trade_count()); f.i(pending_order_count());
        for (int i = 0; i < pending_order_count(); ++i) {
            double stop = kNa, limit = kNa, trail = kNa;
            f.i(pending_order_effective_levels(i, &stop, &limit, &trail));
            f.d(stop); f.d(limit); f.d(trail);
        }
        out_.public_reads.push_back(f.h);
    }

    const int count_, held_;
    const bool margin_, retain_;
    // Test output only: none selects an order or enters adapter state.
    Outcome out_;
    std::uint64_t previous_stop_ = 0, origin_ = 0;
    std::int64_t cycle_ = 0;
};

struct RetentionSwitch {
    explicit RetentionSwitch(bool retain) : previous(ps::detail::retain_retired_rows()) {
        ps::detail::set_retain_retired_rows(retain);
    }
    ~RetentionSwitch() { ps::detail::set_retain_retired_rows(previous); }
    bool previous;
};

inline std::vector<Bar> tape(int count, bool margin) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        Bar bar{117.0, 126.0, 114.0, 115.0, 1.0, kStart + i * kMinute};
        if (i <= 2) bar = {100.0, 100.2, 99.8, 100.0, 1.0, bar.timestamp};
        if (i == 3) bar = {115.0, 121.0, 114.0, 115.0, 1.0, bar.timestamp};
        if (margin && i == count - 2)
            bar = {115.0, 116.0, 114.0, 115.0, 1.0, bar.timestamp};
        if (margin && i == count - 1)
            bar = {115.0, 300.0, 114.0, 300.0, 1.0, bar.timestamp};
        bars.push_back(bar);
    }
    return bars;
}

inline Outcome run(int count, int held, bool margin, bool retain) {
    RetentionSwitch switch_for_run(retain);
    const auto bars = tape(count, margin);
    Host host(count, held, margin, retain);
    host.run(bars.data(), count, "1", "1", false, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

inline void growth(int count) {
    const auto out = run(count, 0, false, false);
    std::printf("DORMANT BOUND N=%d calls=%d settled=%d peak_origin=%zu final_origin=%zu "
                "origin_bound=%zu peak_placement=%zu final_placement=%zu placement_bound=%zu "
                "snapshot_bytes=%zu peak_origin_payload=%zu payload_bound=%zu ru_maxrss_kib=%llu\n",
                count, out.exit_calls, out.settled_exits, out.peak_origin_rows,
                out.final_origin_rows, kOriginRowBound, out.peak_rows, out.final_rows,
                kPlacementRowBound, sizeof(ps::PlacementSnapshot),
                out.peak_origin_rows * sizeof(ps::PlacementSnapshot),
                kOriginRowBound * sizeof(ps::PlacementSnapshot),
                static_cast<unsigned long long>(out.rss_kib));
    std::fflush(stdout);
    require(out.peak_origin_rows > 0, "bound test never observed the family");
    require(out.peak_origin_rows <= kOriginRowBound
            && out.final_origin_rows <= kOriginRowBound, "dormant origin row bound exceeded");
    require(out.peak_rows <= kPlacementRowBound && out.final_rows <= kPlacementRowBound,
            "whole placement row bound exceeded");
}

inline bool same_number(double a, double b) {
    return bits(a) == bits(b);
}

inline bool same_trade(const Trade& a, const Trade& b) {
    return a.entry_time == b.entry_time && a.exit_time == b.exit_time
        && same_number(a.entry_price, b.entry_price) && same_number(a.exit_price, b.exit_price)
        && same_number(a.qty, b.qty) && same_number(a.pnl, b.pnl)
        && same_number(a.pnl_pct, b.pnl_pct) && a.is_long == b.is_long
        && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
        && a.entry_id == b.entry_id && a.entry_comment == b.entry_comment
        && a.exit_comment == b.exit_comment && a.exit_id == b.exit_id
        && a.exit_from_bracket == b.exit_from_bracket && same_number(a.max_runup, b.max_runup)
        && same_number(a.max_drawdown, b.max_drawdown) && same_number(a.commission, b.commission)
        && a.entry_incarnation == b.entry_incarnation && a.open_at_end == b.open_at_end
        && a.close_cause == b.close_cause;
}

inline void margin_controls(const Outcome& out, int held) {
    require(out.latest_trap_seen, "margin adversary was never observed");
    int c_fills = 0, margins = 0;
    for (const auto& trade : out.trades) {
        std::printf("DORMANT TRADE retain=%d held=%d entry=%s exit=%s comment=%s "
                    "bar=%d qty=%.17g price=%.17g open=%d\n",
                    out.retain ? 1 : 0, held, trade.entry_id.c_str(), trade.exit_id.c_str(),
                    trade.exit_comment.c_str(), trade.exit_bar_index, trade.qty, trade.exit_price,
                    trade.open_at_end ? 1 : 0);
        if (trade.open_at_end) continue;
        require(trade.exit_bar_index == held + 4, "premature actual close");
        if (trade.exit_id == "X") {
            require(trade.entry_id == "S" && trade.exit_comment == "C"
                    && trade.qty == 20.0 && trade.exit_price == 120.0,
                    "a superseded held stop revived instead of the pinned result");
            ++c_fills;
        } else {
            require(trade.exit_id == "__margin_call__" && trade.qty > 0.0,
                    "unexpected close in the margin tape");
            ++margins;
        }
    }
    require(c_fills == 1 && margins > 0, "C fill and actual margin call are required");
}

inline void margin_equivalence(int held) {
    require(held >= 2 && held % 2 == 0, "trap needs an even alternating held prefix");
    const auto retained = run(held + 5, held, true, true);
    margin_controls(retained, held);
    const auto erased = run(held + 5, held, true, false);
    margin_controls(erased, held);
    require(erased.public_reads == retained.public_reads, "ERASE/RETAIN public reads differ");
    require(same_number(erased.position, retained.position)
            && same_number(erased.equity, retained.equity), "ERASE/RETAIN final book differs");
    require(erased.trades.size() == retained.trades.size(), "ERASE/RETAIN trade counts differ");
    for (std::size_t i = 0; i < erased.trades.size(); ++i)
        require(same_trade(erased.trades[i], retained.trades[i]), "ERASE/RETAIN trade field differs");
    require(erased.different_witness_seen, "ERASE lost the later different-stop witness");
    require(retained.peak_origin_rows >= static_cast<std::size_t>(held),
            "RETAIN control did not retain the issued ladder");
    std::printf("DORMANT MARGIN held=%d erase_peak=%zu retain_peak=%zu trades=%zu equality=exact\n",
                held, erased.peak_origin_rows, retained.peak_origin_rows, erased.trades.size());
}
} // namespace dormant_retirement
