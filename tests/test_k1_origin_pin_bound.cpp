// CR-150 L19G: K1 costs the askable origins, not the exit's re-issue history.
// This unit uses the real Pine host and kernel. The directed recancel case
// invokes the adapter's pair-hold policy at a live callback (the same policy
// a declined reversal uses), then cancels/re-issues an explicit per-origin
// exit. On the base, every withdrawn dormant lifecycle stays pinned by its
// opened origin. The bound assertion must fail while that origin is OPEN.
// The margin cases also reach the real declined-pair / margin-revival path.
// No report-sink export or new declaration is needed for the red run.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_placement_retention.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
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

struct PairHoldTag {
    using type = void (ps::PineExecutionAdapter::*)(const ps::SourceId&);
    friend type access(PairHoldTag);
};
template struct PrivateAccess<PairHoldTag,
    &ps::PineExecutionAdapter::hold_reversal_pair_brackets>;

int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                          \
} while (false)

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

bool exit_family(ps::PineOrderFamily family) {
    return family == ps::PineOrderFamily::ExitLimit
        || family == ps::PineOrderFamily::ExitStop
        || family == ps::PineOrderFamily::ExitTrail;
}

enum class Pattern { Moving, RecancelMoving, RecancelSame, PartialOriginClose,
                     MarginKeep, MarginCancel, MarginReprice };

bool margin_pattern(Pattern pattern) {
    return pattern == Pattern::MarginKeep || pattern == Pattern::MarginCancel
        || pattern == Pattern::MarginReprice;
}

struct Outcome {
    std::vector<std::uint64_t> observations;
    std::vector<Trade> trades;
    std::string error;
    double open = 0.0;
    double equity = 0.0;
    std::size_t peak_origin_rows = 0;
    std::size_t final_origin_rows = 0;
    int exit_calls = 0;
    int callbacks = 0;
    int margin_calls = 0;
};

class Host final : public ps::PineStrategyHost {
public:
    Host(Pattern pattern, int close_at, bool recording)
        : pattern_(pattern), close_at_(close_at) {
        ps::PineStrategyConfig config;
        config.initial_capital = margin_pattern(pattern) ? 10000.0 : 1000000.0;
        config.default_qty_type = static_cast<int>(margin_pattern(pattern)
            ? QtyType::PERCENT_OF_EQUITY : QtyType::FIXED);
        config.default_qty_value = margin_pattern(pattern) ? 100.0 : 4.0;
        config.pyramiding = pattern == Pattern::PartialOriginClose ? 2 : 1;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(margin_pattern(pattern));
        set_broker_state_hash_recording(recording);
    }

    void on_source_bar(const Bar&) override {
        observe();
        const int i = pine_bar_index();
        if (pattern_ == Pattern::PartialOriginClose) {
            if (i == 0) strategy_entry("L", true, kNa, kNa, 4.0);
            if (i == 1) strategy_entry("R", true, kNa, kNa, 4.0);
            if (i == 2) {
                strategy_exit("X", "L", kNa, 90.0, kNa, kNa, kNa, 100.0, {}, 4.0);
                ++out.exit_calls;
                (adapter_.*access(PairHoldTag{}))("L");
            }
            if (i == 8) strategy_close("L");
            return;
        }
        if (margin_pattern(pattern_)) {
            if (i == 1) strategy_entry("S", false, kNa, kNa, 80.0);
            if (i == 2) {
                strategy_exit("X", "S", kNa, 105.0, kNa, kNa, kNa, 100.0, {}, 80.0);
                ++out.exit_calls;
                strategy_entry("L", true);
                strategy_close("S", "Reverse to Long");
            }
            if (i == 4 && pattern_ != Pattern::MarginKeep) strategy_cancel("X");
            if (pattern_ == Pattern::MarginReprice && i >= 4 && i <= 7
                && live_position_size() < 0.0) {
                strategy_exit("X", "S", kNa, 125.0 + i,
                              kNa, kNa, kNa, 100.0, {}, 80.0);
                ++out.exit_calls;
            }
            return;
        }
        if (i == 0) strategy_entry("L", true, kNa, kNa, 4.0);
        if (i == close_at_) {
            strategy_cancel("X");
            strategy_close_all();
            return;
        }
        if (i >= close_at_ || !(live_position_size() > 0.0)) return;
        if (pattern_ != Pattern::Moving) strategy_cancel("X");
        const double distance = pattern_ == Pattern::RecancelSame
            ? 10.0 : 10.0 + static_cast<double>(i % 7);
        strategy_exit("X", "L", 100.0 + distance, 100.0 - distance,
                      kNa, kNa, kNa, 100.0, {}, 4.0);
        ++out.exit_calls;
        if (pattern_ != Pattern::Moving) {
            // Reach a real lifecycle transition with its real event/frame;
            // do not seed table rows or manufacture request incarnations.
            (adapter_.*access(PairHoldTag{}))("L");
        }
    }

    void observe() {
        ++out.callbacks;
        BrokerStateHashSink f;
        f.d(live_position_size());
        f.d(position_avg_price());
        f.d(current_equity());
        f.i(trade_count());
        const int pending = pending_order_count();
        f.i(pending);
        for (int i = 0; i < pending; ++i) {
            double stop = kNa, limit = kNa, trail = kNa;
            f.i(pending_order_effective_levels(i, &stop, &limit, &trail));
            f.d(stop); f.d(limit); f.d(trail);
        }
        out.observations.push_back(f.h);
        // Count the growing site itself, independent of the other L19 terms
        // (admission, unfolded ids, receipts, chart/curve/trade storage).
        const std::size_t rows = origin_rows();
        if (live_position_size() != 0.0)
            out.peak_origin_rows = std::max(out.peak_origin_rows, rows);
    }

    std::size_t origin_rows() const {
        std::size_t count = 0;
        for (const auto& row : adapter_.*access(PlacementTag{})) {
            if (exit_family(row.second.family)
                && row.second.bracket_origin.incarnation != 0
                && row.second.legs.target().incarnation != 0) ++count;
        }
        return count;
    }

    Outcome finish() {
        out.error = last_error();
        out.open = live_position_size();
        out.equity = current_equity();
        out.final_origin_rows = origin_rows();
        for (int i = 0; i < trade_count(); ++i) {
            out.trades.push_back(get_trade(i));
            if (out.trades.back().exit_id == "__margin_call__") ++out.margin_calls;
        }
        return out;
    }

    void hash_host_extension(BrokerStateHashSink& f) const override {
        ps::PineStrategyHost::hash_host_extension(f);
        f.u(static_cast<std::uint64_t>(pattern_)); f.i(close_at_);
        // The observation recorder is a test output, never read by the script.
    }

private:
    Pattern pattern_;
    int close_at_;
    Outcome out;
};

struct RetentionSwitch {
    explicit RetentionSwitch(bool retain) : previous(ps::detail::retain_retired_rows()) {
        ps::detail::set_retain_retired_rows(retain);
    }
    ~RetentionSwitch() { ps::detail::set_retain_retired_rows(previous); }
    bool previous;
};

std::vector<Bar> flat_bars(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 100.5, 99.5, 100.0, 1.0, kStart + i * 60000LL});
    return bars;
}

std::vector<Bar> margin_bars() {
    std::vector<Bar> bars;
    const auto add = [&](double o, double h, double l, double c) {
        bars.push_back({o, h, l, c, 1.0,
            kStart + static_cast<std::int64_t>(bars.size()) * 86400000LL});
    };
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.5, 99.7, 100.5);
    add(101.5, 103.0, 100.8, 102.0);
    for (int i = 0; i < 4; ++i) add(102.0, 102.5, 101.5, 102.0);
    add(102.0, 116.0, 101.8, 115.0);
    add(115.0, 115.5, 114.5, 115.0);
    add(115.0, 115.5, 114.5, 115.0);
    return bars;
}

Outcome run(Pattern pattern, int script_bars, int ratio, bool magnifier,
            bool retain, bool close, bool recording) {
    RetentionSwitch switch_for_run(retain);
    Host host(pattern, close ? script_bars - 8 : std::numeric_limits<int>::max(), recording);
    const auto bars = margin_pattern(pattern) ? margin_bars() : flat_bars(script_bars * ratio);
    host.run(bars.data(), static_cast<int>(bars.size()),
             margin_pattern(pattern) ? "1D" : "1", ratio == 1 ? "" : "3",
             magnifier, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

std::uint64_t trade_digest(const std::vector<Trade>& trades) {
    BrokerStateHashSink f;
    f.u(trades.size());
    for (const auto& t : trades) {
        f.i(t.entry_time); f.i(t.exit_time); f.d(t.entry_price); f.d(t.exit_price);
        f.d(t.qty); f.d(t.pnl); f.d(t.pnl_pct); f.b(t.is_long);
        f.i(t.entry_bar_index); f.i(t.exit_bar_index); f.s(t.entry_id); f.s(t.exit_id);
        f.s(t.entry_comment); f.s(t.exit_comment); f.b(t.exit_from_bracket);
        f.d(t.max_runup); f.d(t.max_drawdown); f.d(t.commission);
        f.u(t.entry_incarnation); f.b(t.open_at_end);
        f.u(static_cast<std::uint64_t>(t.close_cause));
    }
    return f.h;
}

void same_public_result(const Outcome& erased, const Outcome& retained) {
    CHECK(erased.error.empty());
    CHECK(retained.error.empty());
    CHECK(erased.observations == retained.observations);
    CHECK(trade_digest(erased.trades) == trade_digest(retained.trades));
    CHECK(erased.open == retained.open);
    CHECK(erased.equity == retained.equity);
}

void k1_is_bounded_while_the_origin_is_open() {
    for (Pattern pattern : {Pattern::Moving, Pattern::RecancelMoving, Pattern::RecancelSame}) {
        for (int ratio : {1, 3}) for (bool magnifier : {false, true}) {
            const auto small = run(pattern, 48, ratio, magnifier, false, false, false);
            const auto large = run(pattern, 192, ratio, magnifier, false, false, false);
            const auto retained = run(pattern, 192, ratio, magnifier, true, false, false);
            std::printf("K1 pattern=%d ratio=%d magnifier=%d: origin rows %zu -> %zu, retained %zu\n",
                        static_cast<int>(pattern), ratio, magnifier ? 1 : 0,
                        small.peak_origin_rows, large.peak_origin_rows, retained.peak_origin_rows);
            same_public_result(large, retained);
            CHECK(small.error.empty());
            CHECK(large.open == 4.0);
            CHECK(large.exit_calls >= 180);
            CHECK(large.peak_origin_rows > 0);
            // A stop + limit per open origin; allow the just-retired pair
            // between its callback and the next bar-open sweep.
            CHECK(large.peak_origin_rows <= 4);
            CHECK(large.peak_origin_rows <= small.peak_origin_rows + 2);
            if (pattern != Pattern::Moving) {
                // Non-vacuous red control: the unbounded base retains the
                // withdrawn dormant rows under the SAME opened origin.
                CHECK(retained.peak_origin_rows >= 180);
            }
        }
    }
}

void the_origin_pin_ends_when_its_lot_closes() {
    for (bool recording : {false, true}) {
        const auto erased = run(Pattern::RecancelMoving, 80, 1, false, false, true, recording);
        const auto retained = run(Pattern::RecancelMoving, 80, 1, false, true, true, recording);
        same_public_result(erased, retained);
        CHECK(erased.open == 0.0);
        CHECK(!erased.trades.empty());
        CHECK(erased.final_origin_rows == 0);
    }
    // One origin closes while another lot keeps the position cycle alive.
    for (int ratio : {1, 3}) for (bool magnifier : {false, true}) {
        const auto erased = run(Pattern::PartialOriginClose, 24, ratio, magnifier,
                                false, false, false);
        const auto retained = run(Pattern::PartialOriginClose, 24, ratio, magnifier,
                                  true, false, false);
        same_public_result(erased, retained);
        CHECK(erased.open == 4.0);
        CHECK(erased.trades.size() == 1);
        CHECK(erased.final_origin_rows == 0);
        CHECK(retained.final_origin_rows > 0);
    }
}

void margin_revival_keeps_its_decision() {
    for (Pattern pattern : {Pattern::MarginKeep, Pattern::MarginCancel, Pattern::MarginReprice}) {
        for (bool magnifier : {false, true}) {
            const auto erased = run(pattern, 11, 1, magnifier, false, false, true);
            const auto retained = run(pattern, 11, 1, magnifier, true, false, true);
            same_public_result(erased, retained);
            CHECK(erased.margin_calls > 0);
            const bool through_x = std::any_of(erased.trades.begin(), erased.trades.end(),
                [](const Trade& trade) { return trade.exit_id == "X"; });
            CHECK(through_x == (pattern == Pattern::MarginKeep));
        }
    }
}
} // namespace

int main() {
    k1_is_bounded_while_the_origin_is_open();
    the_origin_pin_ends_when_its_lot_closes();
    margin_revival_keeps_its_decision();
    std::printf("test_k1_origin_pin_bound: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
