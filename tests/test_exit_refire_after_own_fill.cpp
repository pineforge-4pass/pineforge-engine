// Synthetic reduction: a fixed half-entry exit is called on every execution,
// including calc_on_order_fills callbacks, while its entry still has units.
// The two-exit expectations express the measured TradingView rule: a fills
// once, b takes half the remaining position, and close_all takes the rest.
// R-one-exit is an EXTRAPOLATION until the public TradingView reading; it is
// not a measured rule. R-control calls a once and checks the same harness.
// Model: test_coof_reused_id_reentry.cpp (PineStrategyHost configuration/run).
// Unlike that model's command body, this body also runs after every fill.
// No market data or fixture is read. Baseline outcomes require a separate run;
// in particular, the per-origin consumed-leg guard may keep R-one-exit green.

#include <pineforge/bar.hpp>
#include <pineforge/source/exit_payload_readback.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/exit_scratch_versions_production.hpp"
#include "../src/source/pine_command_scratch_production.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using namespace pineforge;

using ExitMemberSignature = void (source::PineExecutionAdapter::*)(
    const source::SourceId&, const source::SourceId&,
    double, double, double, double, double, double,
    const std::string&, double, const std::string&, double, double);
static_assert(std::is_same_v<decltype(&source::PineExecutionAdapter::exit),
                             ExitMemberSignature>,
              "PineExecutionAdapter::exit must retain its original public signature");

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kOpeningQty = 256.0;
constexpr double kEntryPrice = 100.0;
constexpr double kLimit = kEntryPrice + 8.0;
constexpr double kStopB = kEntryPrice - 4.0;
constexpr double kStopA = kEntryPrice - 12.0;
constexpr double kTick = 1.0;
constexpr double kLot = 1.0;
constexpr std::int64_t kStart = 1'735'689'600'000LL;
constexpr std::int64_t kInterval = 15 * 60'000LL;
constexpr int kEntryCallBar = 0;
constexpr int kEntryFillBar = kEntryCallBar + 1;
constexpr int kLimitBar = kEntryFillBar + 2;
constexpr int kStopBBar = 8;
constexpr int kStopABar = 10;
constexpr int kCloseCallBar = 13;
constexpr int kCloseFillBar = kCloseCallBar + 1;
constexpr int kBarCount = kCloseFillBar + 2;
static_assert(kLimitBar < kStopBBar && kStopBBar < kStopABar
              && kStopABar < kCloseCallBar, "exit levels precede the final close");
constexpr const char* kEntryId = "e";
// strategy_close_all()'s report label, not an additional strategy.exit.
constexpr const char* kFinalExitId = "__close__";

struct Row {
    const char* name;
    bool is_long;
    bool two_exits;
    bool call_once;
};

constexpr Row kRows[] = {
    {"R-two-exits", true, true, false},
    {"R-two-exits-short", false, true, false},
    {"R-one-exit", true, false, false},
    {"R-control", true, false, true},
};

double price_for(const Row& row, double long_price) {
    return row.is_long ? long_price : 2.0 * kEntryPrice - long_price;
}

std::vector<Bar> synthetic_bars(const Row& row) {
    struct Prices { double open, high, low, close; };
    const std::array<Prices, kBarCount> prices{{
        {100, 102, 98, 100},                         // 0: signal
        {kEntryPrice, 102, 98, 100},                 // 1: market entry
        {101, 104, 100, 103},                        // 2: neither exit
        {104, kLimit + 2, 103, kLimit + 1},          // 3: a's first limit
        {kLimit + 4, 114, kLimit + 3, 113},          // 4: beyond the limit
        {113, 115, kLimit + 3, 114},                 // 5: beyond the limit
        {114, 116, kLimit + 4, 115},                 // 6: beyond the limit
        {102, 104, 99, 100},                         // 7: neither level
        {100, 101, kStopB - 1, kStopB + 1},          // 8: b's stop
        {97, 99, kStopB - 2, 98},                    // 9: above a's stop
        {96, 98, kStopA - 1, kStopA + 2},            // 10: a's stop
        {kStopA - 2, kStopA + 1, kStopA - 3, 87},   // 11: past that stop
        {90, 93, 89, 92},                            // 12
        {92, 95, 91, 94},                            // 13: close_all call
        {94, 97, 93, 95},                            // 14: market close
        {95, 98, 94, 96},                            // 15: trailing bar
    }};
    std::vector<Bar> bars;
    for (int i = 0; i < kBarCount; ++i) {
        const Prices& p = prices[static_cast<std::size_t>(i)];
        bars.push_back({
            price_for(row, p.open),
            price_for(row, row.is_long ? p.high : p.low),
            price_for(row, row.is_long ? p.low : p.high),
            price_for(row, p.close), 1000.0, kStart + i * kInterval});
    }
    return bars;
}

class ExitProbe final : public source::PineStrategyHost {
public:
    explicit ExitProbe(const Row& row) : row_(row) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        cfg.process_orders_on_close = false;
        cfg.pyramiding = 0;
        cfg.initial_capital = 10'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = kOpeningQty;
        cfg.margin_long = 100.0;
        cfg.margin_short = 100.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(kTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", kLot);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == kEntryCallBar && !entry_sent_) {
            entry_sent_ = true;
            strategy_entry(kEntryId, row_.is_long, kNaN, kNaN, kOpeningQty);
        }
        const double held = std::abs(signed_position_size());
        if (held == 0.0) return;
        // No ordinary-calculation filter: repeat the script's exits in every
        // fill callback as well as in each bar's normal calculation.
        if (!row_.call_once || a_calls == 0) {
            ++a_calls;
            if (first_a_call_bar < 0) first_a_call_bar = pine_bar_index();
            strategy_exit("a", kEntryId, price_for(row_, kLimit),
                          price_for(row_, kStopA), kNaN, kNaN, kNaN,
                          100.0, {}, kOpeningQty / 2.0);
        }
        if (row_.two_exits) {
            strategy_exit("b", kEntryId, kNaN, price_for(row_, kStopB),
                          kNaN, kNaN, kNaN, 100.0, {}, held / 2.0);
        }
        if (pine_bar_index() == kCloseCallBar && !close_sent_) {
            close_sent_ = true;
            strategy_close_all();
        }
    }

    double final_size() const { return signed_position_size(); }
    int a_calls = 0;
    int first_a_call_bar = -1;

private:
    const Row& row_;
    bool entry_sent_ = false;
    bool close_sent_ = false;
};

bool equal_number(const Row& row, const std::string& field,
                  double actual, double expected) {
    if (actual == expected) return true;
    std::printf("FAIL %s: %s actual=%.17g expected=%.17g\n",
                row.name, field.c_str(), actual, expected);
    return false;
}

bool equal_text(const Row& row, const std::string& field,
                const std::string& actual, const std::string& expected) {
    if (actual == expected) return true;
    std::printf("FAIL %s: %s actual=\"%s\" expected=\"%s\"\n",
                row.name, field.c_str(), actual.c_str(), expected.c_str());
    return false;
}

int exit_count(const ExitProbe& probe, const char* id) {
    int count = 0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        if (!trade.open_at_end && trade.entry_id == kEntryId && trade.exit_id == id)
            ++count;
    }
    return count;
}

bool check_leg(const Row& row, const ExitProbe& probe, const char* id,
               double qty, int bar, double price) {
    int matches = 0;
    Trade actual{};
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        if (!trade.open_at_end && trade.entry_id == kEntryId
            && trade.exit_id == id && trade.exit_bar_index == bar) {
            ++matches;
            actual = trade;
        }
    }
    const std::string leg = std::string("leg (") + kEntryId + ", " + id
        + ", bar " + std::to_string(bar) + ")";
    return equal_number(row, leg + " count", matches, 1)
        && equal_number(row, leg + " quantity", actual.qty, qty)
        && equal_number(row, leg + " price", actual.exit_price, price);
}

bool run_row(const Row& row) {
    ExitProbe probe(row);
    const auto bars = synthetic_bars(row);
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);

    // Print the entire list before any comparison, including an unexpected
    // open-at-end row, so the first failure never hides an extra a fill.
    std::printf("%s: trades=%d a_calls=%d first_a_call_bar=%d\n",
                row.name, probe.trade_count(), probe.a_calls, probe.first_a_call_bar);
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& t = probe.get_trade(i);
        std::printf("  exit=%s entry=%s quantity=%.17g bar=%d price=%.17g open=%d\n",
                    t.exit_id.c_str(), t.entry_id.c_str(), t.qty,
                    t.exit_bar_index, t.exit_price, t.open_at_end ? 1 : 0);
    }
    std::fflush(stdout);
    if (!equal_text(row, "run error", probe.last_error(), "")) return false;
    if (!equal_number(row, "first a call bar", probe.first_a_call_bar, kEntryFillBar))
        return false;
    if (row.call_once && !equal_number(row, "a call count", probe.a_calls, 1))
        return false;
    // Check the setup independently before diagnosing a repeated exit.
    double closed_qty = 0.0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& t = probe.get_trade(i);
        const std::string prefix = "trade " + std::to_string(i) + " ";
        if (!equal_text(row, prefix + "entry id", t.entry_id, kEntryId)
            || !equal_number(row, prefix + "entry bar", t.entry_bar_index, kEntryFillBar)
            || !equal_number(row, prefix + "entry price", t.entry_price, kEntryPrice)
            || !equal_number(row, prefix + "long", t.is_long, row.is_long)
            || !equal_number(row, prefix + "open at end", t.open_at_end, false))
            return false;
        closed_qty += t.qty;
    }
    if (!equal_number(row, "total closed quantity", closed_qty, kOpeningQty))
        return false;
    // This is the first behavioural assertion: count ALL of a's closed rows,
    // not just its correct first fill or the total quantity of the entry.
    if (!equal_number(row, "exit a fill count", exit_count(probe, "a"), 1))
        return false;
    if (!check_leg(row, probe, "a", kOpeningQty / 2.0, kLimitBar,
                   price_for(row, kLimit))) return false;
    if (!equal_number(row, "exit b fill count", exit_count(probe, "b"),
                      row.two_exits ? 1 : 0)) return false;
    if (row.two_exits
        && !check_leg(row, probe, "b", kOpeningQty / 4.0, kStopBBar,
                      price_for(row, kStopB))) return false;
    const double final_qty = row.two_exits ? kOpeningQty / 4.0 : kOpeningQty / 2.0;
    if (!equal_number(row, "final close fill count", exit_count(probe, kFinalExitId), 1)
        || !check_leg(row, probe, kFinalExitId, final_qty, kCloseFillBar,
                      bars[kCloseFillBar].open)
        || !equal_number(row, "trade count", probe.trade_count(), row.two_exits ? 3 : 2)
        || !equal_number(row, "final position", probe.final_size(), 0.0))
        return false;
    std::printf("PASS %s\n", row.name);
    return true;
}

// TradingView refire decider (decoded.json 938b430f, checks.json 4e5b07f0,
// qualified cells 0/1/2) and cause-C r2 qualified cells c00
// (k11104), c02, c17, c19 and short c36: after a partial Limit fill, a
// re-issued same-id Stop on that opening does not fill; c25 is the positive
// control where the Stop fills without an earlier fill. This engine gives
// the no-fill outcome as no staged or native Stop. Cross its level below
// and check the remaining two units and complete trade list independently.
constexpr const char* kQueuedRow = "R-queued-predecessor";
constexpr const char* kQueuedEntry = "queued-entry";
constexpr const char* kHolderExit = "holder";
constexpr const char* kConsumedExit = "consumed";
constexpr double kQueuedLimit = 108.0;
constexpr double kQueuedStop = 90.0;
constexpr double kHolderStop = 50.0;

class QueuedPredecessorProbe final : public source::PineStrategyHost {
public:
    QueuedPredecessorProbe() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = false;
        cfg.process_orders_on_close = false;
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 4.0;
        cfg.margin_long = cfg.margin_short = 100.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        // All amounts lie on this quarter-unit grid. The tombstone rule's
        // power-of-ten scope must not mask a leaked queued stop here.
        qty_step_ = 0.25;
        fixture_retain_all_events();
    }

    void expect(const std::string& field, double actual, double expected) {
        if (actual == expected) return;
        std::printf("FAIL %s: %s actual=%.17g expected=%.17g\n",
                    kQueuedRow, field.c_str(), actual, expected);
        ok = false;
    }

    int working(const char* id) const {
        int count = 0;
        for (const auto& row : native_working_requests())
            if (row.definition->request.label == id) ++count;
        return count;
    }

    int pending(const char* id) const {
        int count = 0;
        for (const auto& row : adapter_.fixture_pending_snapshots())
            if (row.snapshot.source_id == id
                && row.snapshot.from_entry == kQueuedEntry) ++count;
        return count;
    }

    source::PineExecutionAdapter::FixturePendingSnapshot observe(
            const char* phase, const char* id, bool staged,
            double requested, double reserved, source::PineOrderFamily family,
            double level) {
        source::PineExecutionAdapter::FixturePendingSnapshot observed;
        int count = 0;
        for (const auto& row : adapter_.fixture_pending_snapshots()) {
            if (row.snapshot.source_id != id
                || row.snapshot.from_entry != kQueuedEntry) continue;
            ++count;
            observed = row;
        }
        const std::string prefix = std::string(phase) + " ";
        expect(prefix + "pending count", count, 1);
        expect(prefix + "staged", observed.staged, staged);
        expect(prefix + "requested units", observed.snapshot.requested_qty, requested);
        expect(prefix + "reserved units", observed.snapshot.projection_remaining_qty,
               reserved);
        expect(prefix + "family", static_cast<int>(observed.snapshot.family),
               static_cast<int>(family));
        expect(prefix + "level", family == source::PineOrderFamily::ExitStop
               ? observed.snapshot.exit_levels.stop : observed.snapshot.exit_levels.limit,
               level);
        return observed;
    }

    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        if (index == 0) strategy_entry(kQueuedEntry, true, kNaN, kNaN, 4.0);
        if (index == 1) {
            expect("opening position", signed_position_size(), 4.0);
            holder(2.0);
            strategy_exit(kConsumedExit, kQueuedEntry, kQueuedLimit, kNaN,
                          kNaN, kNaN, kNaN, 100.0, {}, 2.0);
            observe("initial holder", kHolderExit, true, 2.0, 2.0,
                    source::PineOrderFamily::ExitStop, kHolderStop);
            observe("initial limit", kConsumedExit, true, 2.0, 2.0,
                    source::PineOrderFamily::ExitLimit, kQueuedLimit);
            opening_seen = true;
        }
        if (index == 3) {
            expect("partial fill count before sequence", trade_count(), 1);
            expect("position before sequence", signed_position_size(), 2.0);
            std::int64_t cycle = 0;
            int openings = 0, partials = 0;
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (!fill) continue;
                if (fill->request().label == kQueuedEntry) {
                    ++openings;
                    expect("native opening units", fill->opened_units, 4.0);
                    cycle = fill->cycle_after;
                }
                if (fill->request().label == kConsumedExit) {
                    ++partials;
                    expect("native partial units", fill->closed_units, 2.0);
                    expect("native partial is limit",
                           std::holds_alternative<native_order::Limit>(
                               fill->request().trigger), true);
                }
            }
            expect("native opening count", openings, 1);
            expect("native partial count", partials, 1);
            const auto consumed = adapter_.fixture_consumed_partial_exit_cycles().lookup(
                std::string(kConsumedExit) + '\x1f' + kQueuedEntry);
            expect("consumed marker matches live cycle",
                   cycle > 0 && consumed && *consumed == cycle, true);
            expect("consumed family has no native leg", working(kConsumedExit), 0);
            observe("standing holder", kHolderExit, false, 2.0, 2.0,
                    source::PineOrderFamily::ExitStop, kHolderStop);

            holder(1.0);
            observe("shrunk holder", kHolderExit, false, 1.0, 1.0,
                    source::PineOrderFamily::ExitStop, kHolderStop);
            stop();
            expect("consumed stop reissue is not pending", pending(kConsumedExit), 0);
            expect("consumed stop reissue is not native", working(kConsumedExit), 0);
            holder(2.0);
            const auto expanded = observe("expanded holder", kHolderExit, false, 2.0, 2.0,
                                          source::PineOrderFamily::ExitStop, kHolderStop);
            expect("holder retains its reservation rank",
                   expanded.snapshot.chain_origin_sequence > 0, true);
            expect("expanded holder is native", working(kHolderExit), 1);
            expect("consumed stop remains absent after holder resize",
                   pending(kConsumedExit), 0);
            expect("consumed stop remains non-native after holder resize",
                   working(kConsumedExit), 0);
            expect("position unchanged after reissue", signed_position_size(), 2.0);

            stop();
            expect("repeated consumed stop is not pending", pending(kConsumedExit), 0);
            expect("repeated consumed stop is not native", working(kConsumedExit), 0);
            observe("holder after repeated reissue", kHolderExit, false, 2.0, 2.0,
                    source::PineOrderFamily::ExitStop, kHolderStop);
            sequence_seen = true;
        }
        if (index == 4) {
            stop_touch_seen = bar.low < kQueuedStop && bar.high > kQueuedStop
                && bar.low > kHolderStop;
            expect("later bar crosses only consumed stop", stop_touch_seen, true);
            expect("consumed stop absent when crossed", pending(kConsumedExit), 0);
            expect("consumed stop non-native when crossed", working(kConsumedExit), 0);
            expect("remaining units after stop touch", signed_position_size(), 2.0);
        }
        if (index == 5) strategy_close_all();
    }

    double final_size() const { return signed_position_size(); }
    bool ok = true;
    bool opening_seen = false, sequence_seen = false, stop_touch_seen = false;

private:
    void holder(double qty) {
        strategy_exit(kHolderExit, kQueuedEntry, kNaN, kHolderStop,
                      kNaN, kNaN, kNaN, 100.0, {}, qty);
    }
    void stop() {
        strategy_exit(kConsumedExit, kQueuedEntry, kNaN, kQueuedStop,
                      kNaN, kNaN, kNaN, 100.0, {}, 2.0);
    }
};

bool run_queued_predecessor() {
    QueuedPredecessorProbe probe;
    const std::array<Bar, 8> bars{{
        {100, 101, 99, 100, 1000, kStart},                    // 0: entry call
        {100, 101, 99, 100, 1000, kStart + kInterval},        // 1: entry, exits
        {100, 109, 99, 108, 1000, kStart + 2 * kInterval},    // 2: limit fills 2
        {105, 106, 104, 105, 1000, kStart + 3 * kInterval},   // 3: same-id Stop reissues
        {100, 101, 89, 95, 1000, kStart + 4 * kInterval},     // 4: cross consumed Stop
        {100, 102, 98, 100, 1000, kStart + 5 * kInterval},    // 5: final close call
        {101, 103, 99, 102, 1000, kStart + 6 * kInterval},    // 6: final close
        {102, 103, 100, 101, 1000, kStart + 7 * kInterval},   // 7: trailing bar
    }};
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    probe.expect("run succeeded", probe.last_error().empty(), true);
    probe.expect("opening setup observed", probe.opening_seen, true);
    probe.expect("same-id reissue sequence observed", probe.sequence_seen, true);
    probe.expect("later stop touch observed", probe.stop_touch_seen, true);
    int stops_accepted = 0, stops_filled = 0;
    for (const auto& row : probe.native_events(0)) {
        if (!row.command) continue;
        if (const auto* event = std::get_if<native_order::AcceptedEvent>(&*row.command))
            if (event->request().label == kConsumedExit
                && std::holds_alternative<native_order::Stop>(event->request().trigger))
                ++stops_accepted;
        if (const auto* event =
                std::get_if<native_order::ExecutionAppliedEvent>(&*row.command))
            if (event->request().label == kConsumedExit
                && std::holds_alternative<native_order::Stop>(event->request().trigger))
                ++stops_filled;
    }
    probe.expect("queued stop accepted after source drain", stops_accepted, 0);
    probe.expect("queued stop later fill count", stops_filled, 0);
    double total = 0.0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        std::printf("%s: exit=%s quantity=%.17g bar=%d price=%.17g\n",
                    kQueuedRow, trade.exit_id.c_str(), trade.qty,
                    trade.exit_bar_index, trade.exit_price);
        const std::string prefix = "trade " + std::to_string(i) + " ";
        probe.expect(prefix + "entry id", trade.entry_id == kQueuedEntry, true);
        probe.expect(prefix + "entry bar", trade.entry_bar_index, 1);
        probe.expect(prefix + "entry price", trade.entry_price, 100.0);
        probe.expect(prefix + "long", trade.is_long, true);
        probe.expect(prefix + "open at end", trade.open_at_end, false);
        probe.expect(prefix + "exit id",
                     trade.exit_id == (i == 0 ? kConsumedExit : kFinalExitId), true);
        probe.expect(prefix + "quantity", trade.qty, 2.0);
        probe.expect(prefix + "exit bar", trade.exit_bar_index, i == 0 ? 2 : 6);
        probe.expect(prefix + "exit price", trade.exit_price, i == 0 ? kQueuedLimit : 101.0);
        total += trade.qty;
    }
    probe.expect("trade count", probe.trade_count(), 2);
    probe.expect("total closed units", total, 4.0);
    probe.expect("final position", probe.final_size(), 0.0);
    if (probe.ok) std::printf("PASS %s\n", kQueuedRow);
    return probe.ok;
}

constexpr const char* kFreshEntry = "renewed-entry";
constexpr const char* kFreshExit = "partial";
constexpr double kFreshOpening = 4.0;
constexpr double kFreshAdd = 4.0;
constexpr double kFreshSlice = kFreshOpening / 2.0;
constexpr double kFreshHolder = kFreshOpening - kFreshSlice;
constexpr Row kFreshRows[] = {
    {"R-fresh-add", true, false, false},
    {"R-fresh-add-short", false, false, false},
};

// expectation corrected: declared levels -> raw-path tick boundaries, because
// exit() submits thresholds through exit_limit_trigger() and
// source_trigger_threshold() in pine_adapter.cpp,
// with calc_on_order_fills=false in both new probes. Independently invert
// Q(p) = kTick * floor(p / kTick + 1/2) for these positive, on-grid levels:
// upward reach starts at L - kTick/2; downward reach ends strictly below
// L + kTick/2, so its endpoint is the preceding representable value.
double expected_native_exit_boundary(const Row& row, double long_level, bool is_limit) {
    const double level = price_for(row, long_level);
    const bool upward = row.is_long == is_limit;
    const double boundary = level + (upward ? -kTick : kTick) / 2.0;
    return upward ? boundary : std::nextafter(
        boundary, -std::numeric_limits<double>::infinity());
}

// A refusal must keep the old opening's consumed bracket, without making a
// later opening of the same id inherit it. The position never goes flat.
// FIFO makes the fresh exit close the old opening's remaining units; the
// final market close then owns exactly the added opening's units.
class FreshAddProbe final : public source::PineStrategyHost {
public:
    explicit FreshAddProbe(const Row& row) : row_(row) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = false;
        cfg.process_orders_on_close = false;
        cfg.pyramiding = 2;
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = kFreshOpening;
        cfg.margin_long = cfg.margin_short = 100.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(kTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", kLot);
        fixture_retain_all_events();
    }

    void expect(const std::string& field, double actual, double expected) {
        if (!equal_number(row_, field, actual, expected)) ok = false;
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0)
            strategy_entry(kFreshEntry, row_.is_long, kNaN, kNaN, kFreshOpening);
        if (index == 1) {
            expect("initial position", std::abs(signed_position_size()), kFreshOpening);
            strategy_exit("holder", kFreshEntry, kNaN, price_for(row_, 50.0),
                          kNaN, kNaN, kNaN, 100.0, {}, kFreshHolder);
            partial();
        }
        if (index == 3) {
            expect("own fill before refusal", trade_count(), 1);
            expect("units before refusal", std::abs(signed_position_size()), kFreshHolder);
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (!fill) continue;
                if (fill->request().label == kFreshEntry) {
                    first_origin_ = fill->handle();
                    cycle_ = fill->cycle_after;
                }
                if (fill->request().label == kFreshExit)
                    expect("initial native closed units", fill->closed_units, kFreshSlice);
            }
            const auto consumed = adapter_.fixture_consumed_partial_exit_cycles().lookup(
                std::string(kFreshExit) + '\x1f' + kFreshEntry);
            expect("own fill marked this cycle",
                   cycle_ > 0 && consumed && *consumed == cycle_, true);
            // The holder reserves every remaining unit. This full-size
            // reissue reaches the reservation refusal, not the partial guard.
            partial();
            int refused_legs = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots())
                if (row.snapshot.source_id == kFreshExit) ++refused_legs;
            expect("refused reissue leaves no queued leg", refused_legs, 0);
            for (const auto& row : native_working_requests())
                expect("refused reissue leaves no working leg",
                       row.definition->request.label == kFreshExit, false);
            strategy_entry(kFreshEntry, row_.is_long, kNaN, kNaN, kFreshAdd);
            refusal_seen = true;
        }
        if (index == 4) {
            expect("position after add", std::abs(signed_position_size()),
                   kFreshHolder + kFreshAdd);
            int openings = 0;
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (!fill || fill->request().label != kFreshEntry) continue;
                ++openings;
                expect("add remains in original cycle", fill->cycle_after, cycle_);
                if (fill->handle() != first_origin_) add_origin_ = fill->handle();
            }
            expect("native opening count", openings, 2);
            expect("add has a fresh origin",
                   add_origin_.incarnation != 0 && add_origin_ != first_origin_, true);
            for (const auto& row : native_events(0))
                if (row.ordinal > fresh_events_from_) fresh_events_from_ = row.ordinal;
        }
        if (index >= 4 && index <= 7) partial();
        if (index == 4) {
            int fresh_families = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                if (row.snapshot.source_id != kFreshExit) continue;
                ++fresh_families;
                expect("fresh leg is staged", row.staged, true);
                expect("fresh bracket names entry", row.snapshot.from_entry == kFreshEntry, true);
                expect("fresh leg binds added origin",
                       row.snapshot.bracket_origin == add_origin_, true);
                expect("fresh leg reserved units", row.snapshot.projection_remaining_qty,
                       kFreshSlice);
                expect("fresh bracket limit", row.snapshot.exit_levels.limit,
                       price_for(row_, kLimit));
                expect("fresh bracket stop", row.snapshot.exit_levels.stop,
                       price_for(row_, kStopA));
            }
            // A cycle-wide consumed return would fail this assertion: it
            // suppresses the bracket before considering the added origin.
            // expectation corrected: 2 -> 1, because fixture_pending_snapshots
            // projects staged legs once per family, with both exit levels.
            // Native acceptance below independently requires BOTH requests.
            expect("fresh add bracket projection count", fresh_families, 1);
            add_seen = true;
        }
        if (index == 5) {
            int limits = 0, stops = 0, accepted = 0;
            std::uint64_t group = 0;
            for (const auto& row : native_events(fresh_events_from_)) {
                if (!row.command) continue;
                const auto* event = std::get_if<native_order::AcceptedEvent>(&*row.command);
                if (!event || event->request().label != kFreshExit) continue;
                ++accepted;
                const auto& request = event->request();
                expect("fresh native FIFO owner",
                       std::holds_alternative<native_order::Independent>(request.owner), true);
                if (const auto* limit = std::get_if<native_order::Limit>(&request.trigger)) {
                    ++limits;
                    expect("fresh native limit", limit->price,
                           expected_native_exit_boundary(row_, kLimit, true));
                }
                if (const auto* stop = std::get_if<native_order::Stop>(&request.trigger)) {
                    ++stops;
                    expect("fresh native stop", stop->price,
                           expected_native_exit_boundary(row_, kStopA, false));
                }
                const auto* member = std::get_if<native_order::Member>(&request.group);
                expect("fresh native OCA member", member != nullptr, true);
                if (member) {
                    expect("fresh native OCA group is nonzero", member->group != 0, true);
                    if (group != 0)
                        expect("fresh native legs share OCA group", member->group == group, true);
                    group = member->group;
                    expect("fresh native OCA cancels sibling",
                           member->effect == native_order::GroupEffect::Cancel, true);
                }
            }
            expect("fresh native request count", accepted, 2);
            expect("fresh native limit count", limits, 1);
            expect("fresh native stop count", stops, 1);
            native_bracket_seen = true;
        }
        if (index >= 5 && index <= 8)
            expect("remaining units after fresh exit", std::abs(signed_position_size()),
                   kFreshHolder + kFreshAdd - kFreshSlice);
        if (index == 8) strategy_close_all();
    }

    double final_size() const { return signed_position_size(); }
    bool ok = true;
    bool refusal_seen = false, add_seen = false, native_bracket_seen = false;

private:
    void partial() {
        strategy_exit(kFreshExit, kFreshEntry, price_for(row_, kLimit),
                      price_for(row_, kStopA), kNaN, kNaN, kNaN,
                      100.0, {}, kFreshSlice);
    }
    const Row& row_;
    native_order::RequestHandle first_origin_{}, add_origin_{};
    std::int64_t cycle_ = 0;
    std::uint64_t fresh_events_from_ = 0;
};

bool run_fresh_add(const Row& row) {
    FreshAddProbe probe(row);
    constexpr std::array<std::array<double, 4>, 11> prices{{
        {100, 101, 99, 100},  // 0: first entry call
        {100, 101, 99, 100},  // 1: first entry, holder and partial bracket
        {100, 109, 99, 108},  // 2: first partial limit
        {105, 106, 104, 105}, // 3: refused reissue, same-id add call
        {100, 101, 99, 100},  // 4: add fills, fresh bracket call
        {100, 109, 99, 108},  // 5: fresh partial limit
        {112, 114, 110, 112}, // 6: repeated limit must not refire
        {100, 101, 87, 95},   // 7: repeated stop must not refire
        {100, 102, 98, 100},  // 8: final close call
        {101, 103, 99, 102},  // 9: final close fill
        {102, 103, 100, 101}, // 10: trailing bar
    }};
    std::array<Bar, prices.size()> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto& p = prices[i];
        bars[i] = {price_for(row, p[0]), price_for(row, p[row.is_long ? 1 : 2]),
                   price_for(row, p[row.is_long ? 2 : 1]), price_for(row, p[3]),
                   1000, kStart + static_cast<std::int64_t>(i) * kInterval};
    }
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    probe.expect("run succeeded", probe.last_error().empty(), true);
    probe.expect("refusal observed", probe.refusal_seen, true);
    probe.expect("fresh opening observed", probe.add_seen, true);
    probe.expect("native bracket observed", probe.native_bracket_seen, true);
    probe.expect("trade count", probe.trade_count(), 3);
    double total = 0.0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        const bool final = i == 2;
        const int exit_bar = i == 0 ? 2 : (final ? 9 : 5);
        const double units = final
            ? kFreshOpening + kFreshAdd - 2 * kFreshSlice : kFreshSlice;
        const std::string prefix = "trade " + std::to_string(i) + " ";
        probe.expect(prefix + "entry id", trade.entry_id == kFreshEntry, true);
        probe.expect(prefix + "entry bar", trade.entry_bar_index, final ? 4 : 1);
        probe.expect(prefix + "entry price", trade.entry_price, kEntryPrice);
        probe.expect(prefix + "direction", trade.is_long, row.is_long);
        probe.expect(prefix + "closed", trade.open_at_end, false);
        probe.expect(prefix + "exit id",
                     trade.exit_id == (final ? kFinalExitId : kFreshExit), true);
        probe.expect(prefix + "exit bar", trade.exit_bar_index, exit_bar);
        probe.expect(prefix + "exit price", trade.exit_price,
                     final ? bars[9].open : price_for(row, kLimit));
        probe.expect(prefix + "units", trade.qty, units);
        total += trade.qty;
    }
    probe.expect("total closed units", total, kFreshOpening + kFreshAdd);
    probe.expect("final position", probe.final_size(), 0.0);
    if (probe.ok) std::printf("PASS %s\n", row.name);
    return probe.ok;
}

constexpr const char* kSettledAdd = "added";
constexpr const char* kSettledBase = "carried";
constexpr std::array<const char*, 2> kSettledTargets{{"added-first", "added-second"}};
constexpr double kSettledUnits = 2.0;
constexpr double kSettledSlice = kSettledUnits / 2.0;
constexpr Row kSettledRows[] = {
    {"R-settled-add", true, false, false},
    {"R-settled-add-short", false, false, false},
};

// Ordinary two-target calls, including entry() followed immediately by both
// exits. The first Add is fully closed by its OWN stops; a later named entry
// keeps the position nonflat. Thus no consumed origin remains in Add.opened.
// The new Add's brackets must also accept a level update after its fill.
// The old and new openings have distinct lifetimes within one position cycle.
class SettledAddProbe final : public source::PineStrategyHost {
public:
    explicit SettledAddProbe(const Row& row) : row_(row) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = false;
        cfg.process_orders_on_close = false;
        cfg.pyramiding = 2;
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = kSettledUnits;
        cfg.margin_long = cfg.margin_short = 100.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(kTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", kLot);
        fixture_retain_all_events();
    }

    void expect(const std::string& field, double actual, double expected) {
        if (!equal_number(row_, field, actual, expected)) ok = false;
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0)
            strategy_entry(kSettledAdd, row_.is_long, kNaN, kNaN, kSettledUnits);
        if (index == 1) {
            expect("old Add position", std::abs(signed_position_size()), kSettledUnits);
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* event =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (event && event->request().label == kSettledAdd) {
                    old_origin = event->handle();
                    cycle_ = event->cycle_after;
                }
            }
            strategy_entry(kSettledBase, row_.is_long, kNaN, kNaN, kSettledUnits);
            targets(0.0, kStopA);
        }
        if (index == 3) {
            expect("old Add closed in two slices", trade_count(), 2);
            expect("other entry keeps position open", std::abs(signed_position_size()),
                   kSettledUnits);
            expect("old Add no longer in opened roster",
                   adapter_.retirement_population().opened, 1);
            int lots = 0;
            for (const auto& lot : native_open_lots(kNaN)) {
                ++lots;
                expect("only carried entry remains", lot.entry_label == kSettledBase, true);
            }
            expect("one carried lot", lots, 1);
            for (const auto* id : kSettledTargets) {
                const auto consumed = adapter_.fixture_consumed_partial_exit_cycles().lookup(
                    std::string(id) + '\x1f' + kSettledAdd);
                expect(std::string(id) + " old marker remains in live cycle",
                       cycle_ > 0 && consumed && *consumed == cycle_, true);
            }
            for (const auto& row : native_working_requests())
                for (const auto* id : kSettledTargets)
                    expect("old target is not working",
                           row.definition->request.label == id, false);
            strategy_entry(kSettledAdd, row_.is_long, kNaN, kNaN, kSettledUnits);
            targets(0.0, kStopA);
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* event = std::get_if<native_order::AcceptedEvent>(&*row.command);
                if (event && event->request().label == kSettledAdd)
                    fresh_origin = event->handle();
            }
            expect("new Add has distinct pending provenance",
                   fresh_origin.incarnation != 0 && fresh_origin != old_origin, true);
            projections(true, 0.0, kStopA);
            settled_seen = true;
        }
        if (index == 4) {
            expect("new Add opens beside carried entry", std::abs(signed_position_size()),
                   2 * kSettledUnits);
            int openings = 0;
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* event =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (!event || event->opened_units == 0.0) continue;
                ++openings;
                expect("all openings share original cycle", event->cycle_after, cycle_);
                expect("native opening units", std::abs(event->opened_units), kSettledUnits);
                if (event->request().label == kSettledBase) base_origin = event->handle();
                if (event->handle() == fresh_origin)
                    expect("new Add opens on intended bar",
                           event->interval_open_ms(), kStart + 4 * kInterval);
            }
            expect("three native openings", openings, 3);
            targets(2.0, kStopA + 2.0);
            projections(false, 2.0, kStopA + 2.0);
            for (std::size_t i = 0; i < kSettledTargets.size(); ++i) {
                int limits = 0, stops = 0;
                for (const auto& row : native_working_requests()) {
                    const auto& request = row.definition->request;
                    if (request.label != kSettledTargets[i]) continue;
                    expect("updated bracket uses FIFO",
                           std::holds_alternative<native_order::Independent>(request.owner), true);
                    if (const auto* limit = std::get_if<native_order::Limit>(&request.trigger)) {
                        ++limits;
                        expect("updated native target", limit->price,
                               expected_native_exit_boundary(row_, kLimit + 8.0 * i + 2.0,
                                                             true));
                    }
                    if (const auto* stop = std::get_if<native_order::Stop>(&request.trigger)) {
                        ++stops;
                        expect("updated native protection", stop->price,
                               expected_native_exit_boundary(row_, kStopA + 2.0, false));
                    }
                }
                expect("one updated native limit", limits, 1);
                expect("one updated native stop", stops, 1);
            }
            update_seen = true;
        }
        if (index >= 5 && index <= 8) {
            expect("remaining new Add units", std::abs(signed_position_size()), kSettledUnits);
            if (index <= 7) targets(2.0, kStopA + 2.0);
        }
        if (index == 8) strategy_close_all();
    }

    double final_size() const { return signed_position_size(); }
    bool ok = true, settled_seen = false, update_seen = false;
    native_order::RequestHandle old_origin{}, base_origin{}, fresh_origin{};

private:
    void targets(double offset, double stop) {
        for (std::size_t i = 0; i < kSettledTargets.size(); ++i)
            strategy_exit(kSettledTargets[i], kSettledAdd,
                          price_for(row_, kLimit + 8.0 * i + offset), price_for(row_, stop),
                          kNaN, kNaN, kNaN, 100.0, {}, kSettledSlice);
    }
    void projections(bool staged, double offset, double stop) {
        for (std::size_t i = 0; i < kSettledTargets.size(); ++i) {
            int count = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                const auto& snapshot = row.snapshot;
                if (snapshot.source_id != kSettledTargets[i]) continue;
                ++count;
                expect("target stage", row.staged, staged);
                expect("target names Add", snapshot.from_entry == kSettledAdd, true);
                expect("target binds new Add", snapshot.bracket_origin == fresh_origin, true);
                expect("target slice", snapshot.projection_remaining_qty, kSettledSlice);
                expect("target limit", snapshot.exit_levels.limit,
                       price_for(row_, kLimit + 8.0 * i + offset));
                expect("target protection", snapshot.exit_levels.stop, price_for(row_, stop));
            }
            expect("target projections (staged family or working legs)", count, staged ? 1 : 2);
        }
    }
    const Row& row_;
    std::int64_t cycle_ = 0;
};

bool run_settled_add(const Row& row) {
    SettledAddProbe probe(row);
    constexpr std::array<std::array<double, 4>, 11> prices{{
        {100, 101, 99, 100}, // 0: old Add call
        {100, 101, 99, 100}, // 1: old Add fill, carried entry and two targets
        {100, 101, 87, 95},  // 2: carried entry fill, old Add fully stopped at 88
        {100, 101, 99, 100}, // 3: new Add and both targets in the same callback
        {100, 101, 99, 100}, // 4: new Add fill, update both stops to 90
        {100, 101, 89, 95},  // 5: crosses updated 90, never the former 88
        {112, 120, 110, 116},// 6: reissues must not refire their limits
        {100, 101, 87, 95},  // 7: reissues must not refire their stops
        {100, 102, 98, 100}, // 8: final close call
        {101, 103, 99, 102}, // 9: final close fill
        {102, 103, 100, 101},// 10: trailing bar
    }};
    std::array<Bar, prices.size()> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto& p = prices[i];
        bars[i] = {price_for(row, p[0]), price_for(row, p[row.is_long ? 1 : 2]),
                   price_for(row, p[row.is_long ? 2 : 1]), price_for(row, p[3]),
                   1000, kStart + static_cast<std::int64_t>(i) * kInterval};
    }
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    probe.expect("run succeeded", probe.last_error().empty(), true);
    probe.expect("settled old Add observed", probe.settled_seen, true);
    probe.expect("fresh bracket update observed", probe.update_seen, true);
    probe.expect("trade count", probe.trade_count(), 5);
    double total = 0.0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const auto& trade = probe.get_trade(i);
        const bool old = i < 2, final = i == 4;
        const auto origin = old ? probe.old_origin
            : (final ? probe.fresh_origin : probe.base_origin);
        const std::string prefix = "trade " + std::to_string(i) + " ";
        probe.expect(prefix + "entry id",
                     trade.entry_id == (old || final ? kSettledAdd : kSettledBase), true);
        probe.expect(prefix + "entry incarnation", trade.entry_incarnation == origin.incarnation, true);
        probe.expect(prefix + "entry bar", trade.entry_bar_index, old ? 1 : (final ? 4 : 2));
        probe.expect(prefix + "entry price", trade.entry_price, kEntryPrice);
        probe.expect(prefix + "direction", trade.is_long, row.is_long);
        probe.expect(prefix + "closed", trade.open_at_end, false);
        probe.expect(prefix + "exit id",
                     trade.exit_id == (final ? kFinalExitId : kSettledTargets[i % 2]), true);
        probe.expect(prefix + "exit bar", trade.exit_bar_index, old ? 2 : (final ? 9 : 5));
        probe.expect(prefix + "exit price", trade.exit_price,
                     final ? bars[9].open : price_for(row, old ? kStopA : kStopA + 2.0));
        probe.expect(prefix + "units", trade.qty, final ? kSettledUnits : kSettledSlice);
        total += trade.qty;
    }
    probe.expect("total closed units", total, 3 * kSettledUnits);
    probe.expect("final position", probe.final_size(), 0.0);
    if (probe.ok) std::printf("PASS %s\n", row.name);
    return probe.ok;
}

struct CancelAddRow {
    const char* name;
    bool all_na;
};

constexpr CancelAddRow kCancelAddRows[] = {
    {"R-cancel-add", false},
    {"R-na-cancel-add", true},
};
constexpr const char* kCancelEntry = "L";
constexpr const char* kCancelExit = "X";
constexpr double kCancelOpening = 6.0;
constexpr double kCancelSlice = 2.0;
constexpr double kCancelAdd = 2.0;

// Both cancellations remove the family after its first partial fill. A new
// entry may receive its own bracket, but must not re-arm the consumed origin.
// FIFO still debits the oldest opening when that new bracket fills. The add
// opens on the bar its bracket fills: that leg is the add's own exit, so its
// two units are the whole of the exit there and no second fill covers the add.
class CancelAddProbe final : public source::PineStrategyHost {
public:
    explicit CancelAddProbe(const CancelAddRow& row)
        : row_{row.name, true, false, false}, all_na_(row.all_na) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = false;
        cfg.process_orders_on_close = false;
        cfg.pyramiding = 2;
        cfg.close_entries_rule_any = false;
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = kCancelOpening;
        cfg.margin_long = cfg.margin_short = 100.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(kTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", kLot);
        fixture_retain_all_events();
    }

    void expect(const std::string& field, double actual, double expected) {
        if (!equal_number(row_, field, actual, expected)) ok = false;
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0)
            strategy_entry(kCancelEntry, true, kNaN, kNaN, kCancelOpening);
        if (index == 1) {
            expect("initial position", signed_position_size(), kCancelOpening);
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (fill && fill->request().label == kCancelEntry) {
                    old_origin = fill->handle();
                    cycle_ = fill->cycle_after;
                    expect("initial native opening units", fill->opened_units, kCancelOpening);
                }
            }
            expect("initial origin exists", old_origin.incarnation != 0, true);
        }
        if (index == 3) {
            expect("first exit filled once", trade_count(), 1);
            expect("old opening retains four units", signed_position_size(),
                   kCancelOpening - kCancelSlice);
            const auto consumed = adapter_.fixture_consumed_partial_exit_cycles().lookup(
                std::string(kCancelExit) + '\x1f' + kCancelEntry);
            expect("first exit consumed in current cycle",
                   cycle_ > 0 && consumed && *consumed == cycle_, true);
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (fill && fill->request().label == kCancelExit)
                    consumed_leg_ = fill->handle();
            }
            // Preserve this order: cancel, add, then the ordinary finite X
            // call below. The all-na call is not the last exit request.
            if (all_na_)
                strategy_exit(kCancelExit, kCancelEntry, kNaN, kNaN,
                              kNaN, kNaN, kNaN, 100.0, {}, kCancelSlice);
            else
                strategy_cancel_all();
            int remaining = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots())
                if (row.snapshot.source_id == kCancelExit) ++remaining;
            expect("cancellation leaves no exit projection", remaining, 0);
            expect("cancellation retains position", signed_position_size(),
                   kCancelOpening - kCancelSlice);
            strategy_entry(kCancelEntry, true, kNaN, kNaN, kCancelAdd);
            for (const auto& row : native_events(0))
                if (row.ordinal > replacement_events_from_)
                    replacement_events_from_ = row.ordinal;
        }
        if (index >= 1) finite_exit();
        if (index == 3) {
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* accepted =
                    std::get_if<native_order::AcceptedEvent>(&*row.command);
                if (accepted && accepted->request().label == kCancelEntry)
                    fresh_origin = accepted->handle();
            }
            expect("add has distinct pending origin",
                   fresh_origin.incarnation != 0 && fresh_origin != old_origin, true);
            int projections = 0, old_brackets = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                if (row.snapshot.source_id != kCancelExit) continue;
                ++projections;
                if (row.snapshot.bracket_origin == old_origin) ++old_brackets;
            }
            // First regression assertion: cancellation must not erase the
            // old origin's ineligibility when the pending add admits X.
            expect("consumed opening rearmed", old_brackets, 0);
            expect("one replacement projection", projections, 1);
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                const auto& snapshot = row.snapshot;
                if (snapshot.source_id != kCancelExit) continue;
                expect("replacement names entry", snapshot.from_entry == kCancelEntry, true);
                expect("replacement binds add", snapshot.bracket_origin == fresh_origin, true);
                expect("replacement reserves two units",
                       snapshot.projection_remaining_qty, kCancelSlice);
                expect("replacement limit", snapshot.exit_levels.limit, kLimit);
            }
            cancellation_seen = true;
        }
        if (index == 4) {
            int openings = 0, accepted = 0, fills = 0;
            double closed = 0.0;
            native_order::RequestHandle accepted_leg{}, filled_leg{};
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                if (const auto* fill =
                        std::get_if<native_order::ExecutionAppliedEvent>(&*row.command)) {
                    if (fill->request().label == kCancelEntry) {
                        ++openings;
                        expect("openings retain cycle", fill->cycle_after, cycle_);
                        if (fill->handle() == fresh_origin) {
                            expect("add opens on bar four", fill->interval_open_ms(),
                                   kStart + 4 * kInterval);
                            expect("native add units", fill->opened_units, kCancelAdd);
                        }
                    }
                    if (fill->request().label == kCancelExit
                        && fill->interval_open_ms() == kStart + 4 * kInterval) {
                        ++fills;
                        closed += fill->closed_units;
                        filled_leg = fill->handle();
                    }
                }
                if (row.ordinal <= replacement_events_from_) continue;
                if (const auto* event =
                        std::get_if<native_order::AcceptedEvent>(&*row.command)) {
                    if (event->request().label != kCancelExit) continue;
                    ++accepted;
                    accepted_leg = event->handle();
                    expect("replacement is a new native leg",
                           accepted_leg != consumed_leg_, true);
                    const auto* limit =
                        std::get_if<native_order::Limit>(&event->request().trigger);
                    expect("replacement is native limit", limit != nullptr, true);
                    if (limit)
                        expect("native replacement threshold", limit->price,
                               expected_native_exit_boundary(row_, kLimit, true));
                }
            }
            expect("two native openings", openings, 2);
            expect("one fresh native exit accepted", accepted, 1);
            expect("one fresh native exit filled", fills, 1);
            expect("bar four closes only fresh slice", closed, kCancelSlice);
            expect("filled leg is accepted replacement", filled_leg == accepted_leg, true);
            expect("two partial trades", trade_count(), 2);
            fresh_fill_seen = true;
        }
        if (index == 4 || index == 5)
            expect("position after fresh slice", signed_position_size(),
                   kCancelOpening + kCancelAdd - 2 * kCancelSlice);
        if (index == 5) strategy_close_all();
    }

    double final_size() const { return signed_position_size(); }
    bool ok = true, cancellation_seen = false, fresh_fill_seen = false;
    native_order::RequestHandle old_origin{}, fresh_origin{};

private:
    void finite_exit() {
        strategy_exit(kCancelExit, kCancelEntry, kLimit, kNaN,
                      kNaN, kNaN, kNaN, 100.0, {}, kCancelSlice);
    }
    const Row row_;
    const bool all_na_;
    std::int64_t cycle_ = 0;
    std::uint64_t replacement_events_from_ = 0;
    native_order::RequestHandle consumed_leg_{};
};

bool run_cancel_add(const CancelAddRow& row) {
    CancelAddProbe probe(row);
    const std::array<Bar, 8> bars{{
        {100, 101, 99, 100, 1000, kStart},                  // 0: entry call
        {100, 101, 99, 100, 1000, kStart + kInterval},      // 1: entry, X
        {100, 109, 99, 108, 1000, kStart + 2 * kInterval},  // 2: X consumes 2
        {105, 106, 104, 105, 1000, kStart + 3 * kInterval}, // 3: cancel, add, X
        {100, 109, 99, 108, 1000, kStart + 4 * kInterval},  // 4: add, fresh X
        {112, 114, 110, 112, 1000, kStart + 5 * kInterval}, // 5: no refire, close
        {101, 103, 99, 102, 1000, kStart + 6 * kInterval},  // 6: final close
        {102, 103, 100, 101, 1000, kStart + 7 * kInterval}, // 7: trailing bar
    }};
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    probe.expect("run succeeded", probe.last_error().empty(), true);
    probe.expect("cancellation and finite reissue observed", probe.cancellation_seen, true);
    probe.expect("fresh native fill observed", probe.fresh_fill_seen, true);
    probe.expect("trade count", probe.trade_count(), 4);
    double total = 0.0, fresh_closed = 0.0, old_final = 0.0, add_final = 0.0;
    int fresh_trades = 0, final_trades = 0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const auto& trade = probe.get_trade(i);
        const std::string prefix = "trade " + std::to_string(i) + " ";
        const bool final = trade.exit_id == kFinalExitId;
        const bool add = trade.entry_incarnation == probe.fresh_origin.incarnation;
        probe.expect(prefix + "entry id", trade.entry_id == kCancelEntry, true);
        probe.expect(prefix + "known entry origin",
                     trade.entry_incarnation == probe.old_origin.incarnation || add, true);
        probe.expect(prefix + "entry bar", trade.entry_bar_index, add ? 4 : 1);
        probe.expect(prefix + "entry price", trade.entry_price, kEntryPrice);
        probe.expect(prefix + "long", trade.is_long, true);
        probe.expect(prefix + "closed", trade.open_at_end, false);
        probe.expect(prefix + "exit id", final || trade.exit_id == kCancelExit, true);
        probe.expect(prefix + "units", trade.qty, kCancelSlice);
        probe.expect(prefix + "exit price", trade.exit_price, final ? bars[6].open : kLimit);
        if (final) {
            ++final_trades;
            probe.expect(prefix + "close bar", trade.exit_bar_index, 6);
            (add ? add_final : old_final) += trade.qty;
        } else {
            probe.expect(prefix + "FIFO closes old origin", add, false);
            probe.expect(prefix + "partial bar",
                         trade.exit_bar_index == 2 || trade.exit_bar_index == 4, true);
            if (trade.exit_bar_index == 4) {
                ++fresh_trades;
                fresh_closed += trade.qty;
            }
        }
        total += trade.qty;
    }
    probe.expect("bar four exit trade count", fresh_trades, 1);
    probe.expect("bar four exit trade units", fresh_closed, kCancelSlice);
    probe.expect("final close trade count", final_trades, 2);
    probe.expect("old origin final remaining units", old_final,
                 kCancelOpening - 2 * kCancelSlice);
    probe.expect("add final remaining units", add_final, kCancelAdd);
    probe.expect("total closed units", total, kCancelOpening + kCancelAdd);
    probe.expect("final position", probe.final_size(), 0.0);
    if (probe.ok) std::printf("PASS %s\n", row.name);
    return probe.ok;
}

constexpr Row kCoverTimedRow{"R-cover-timed-add", true, false, false};
constexpr const char* kCoverSeed = "seed";
constexpr const char* kCoverExit = "seed-exit";
constexpr const char* kTimedAdd = "add";
constexpr std::array<const char*, 2> kTimedExits{{"left", "right"}};
constexpr double kCoverLimit = 3660.0;
constexpr double kTimedStop = 3647.85;
constexpr double kTimedTick = 0.001;
constexpr std::int64_t kTimedEntryMs = 1'757'685'600'000LL; // 2025-09-12 14:00 UTC
constexpr std::int64_t kTimedExitMs = 1'757'688'300'000LL;  // 2025-09-12 14:45 UTC
constexpr int kTimedEntryBar = 5, kTimedExitBar = 8;

// Synthetic prefix: a full reissue arms a same-bar add's leg; its FIFO fill
// drains the older opening, so the main-compatible cover must close the add.
// The timed suffix declares a new entry and BOTH one-unit exits together,
// before the entry fills. Its stop is first crossed three bars after entry.
// The prefix is a constructed causal witness, not a recovered script trace.
class CoverTimedAddProbe final : public source::PineStrategyHost {
public:
    CoverTimedAddProbe() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = false;
        cfg.process_orders_on_close = false;
        cfg.pyramiding = 2;
        cfg.close_entries_rule_any = false;
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 2.0;
        cfg.margin_long = cfg.margin_short = 100.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(kTimedTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", kLot);
        fixture_retain_all_events();
    }

    void expect(const std::string& field, double actual, double expected) {
        if (!equal_number(kCoverTimedRow, field, actual, expected)) ok = false;
    }
    void setup_expect(const std::string& field, double actual, double expected) {
        if (!equal_number(kCoverTimedRow, "setup " + field, actual, expected))
            setup_ok = false;
    }
    void price(const std::string& field, double actual, double expected,
               bool setup = false) {
        // Only binary64 representation noise; this is one millionth of a tick.
        if (std::isfinite(actual) && std::abs(actual - expected) <= 1e-9) return;
        if (setup) setup_expect(field, actual, expected);
        else expect(field, actual, expected);
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0)
            strategy_entry(kCoverSeed, true, kNaN, kNaN, 2.0);
        if (index == 1) {
            setup_expect("initial seed position", signed_position_size(), 2.0);
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (!fill || fill->request().label != kCoverSeed
                    || !(fill->opened_units > 0.0)) continue;
                old_origin = fill->handle();
                cycle_ = fill->cycle_after;
                setup_expect("initial native opening units", fill->opened_units, 2.0);
            }
            setup_expect("initial origin exists", old_origin.incarnation != 0, true);
            seed_exit();
        }
        if (index == 3) {
            setup_expect("one initial partial trade", trade_count(), 1);
            setup_expect("full reissue live basis", signed_position_size(), 1.0);
            if (trade_count() == 1) {
                const auto& trade = get_trade(0);
                setup_expect("initial partial owns old origin",
                             trade.entry_incarnation == old_origin.incarnation, true);
                setup_expect("initial partial units", trade.qty, 1.0);
                setup_expect("initial partial bar", trade.exit_bar_index, 2);
                price("initial partial price", trade.exit_price, kCoverLimit, true);
            }
            const auto consumed = adapter_.fixture_consumed_partial_exit_cycles().lookup(
                std::string(kCoverExit) + '\x1f' + kCoverSeed);
            setup_expect("initial partial consumed this cycle",
                         cycle_ > 0 && consumed && *consumed == cycle_, true);
            prefix_events_from = cursor();
            strategy_entry(kCoverSeed, true, kNaN, kNaN, 1.0);
            // qty 1 covers the ONE live unit: requires_fresh_origin is false.
            // The pending one-unit add must not turn this into a partial call.
            seed_exit();
            for (const auto& row : native_events(prefix_events_from)) {
                if (!row.command) continue;
                const auto* accepted =
                    std::get_if<native_order::AcceptedEvent>(&*row.command);
                if (accepted && accepted->request().label == kCoverSeed)
                    seed_add_origin = accepted->handle();
            }
            setup_expect("distinct pending seed add",
                         seed_add_origin.incarnation != 0 && seed_add_origin != old_origin, true);
            int projections = 0, revived = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                const auto& snapshot = row.snapshot;
                if (snapshot.source_id != kCoverExit) continue;
                ++projections;
                if (snapshot.bracket_origin == old_origin) ++revived;
                setup_expect("reissue binds pending seed add",
                             snapshot.bracket_origin == seed_add_origin, true);
                setup_expect("reissue reserves one unit", snapshot.projection_remaining_qty, 1.0);
                price("reissue limit", snapshot.exit_levels.limit, kCoverLimit, true);
            }
            setup_expect("one fresh seed leg", projections, 1);
            setup_expect("consumed seed never revived", revived, 0);
        }
        if (index == 4) {
            int openings = 0, priced = 0;
            double covered = 0.0;
            for (const auto& row : native_events(prefix_events_from)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (!fill) continue;
                if (fill->handle() == seed_add_origin && fill->opened_units > 0.0) {
                    ++openings;
                    setup_expect("seed add opens one unit", fill->opened_units, 1.0);
                    setup_expect("seed add stays in old cycle", fill->cycle_after, cycle_);
                    setup_expect("seed add bar", fill->interval_index(), 4);
                }
                if (fill->request().label != kCoverExit) continue;
                if (std::holds_alternative<native_order::Limit>(fill->request().trigger)) {
                    ++priced;
                    setup_expect("full reissue closes one unit", fill->closed_units, 1.0);
                    setup_expect("full reissue fill bar", fill->interval_index(), 4);
                    price("full reissue fill price", fill->resolved_price, kCoverLimit, true);
                    setup_expect("full reissue closes one trade", fill->closed_trade_count, 1);
                    if (fill->first_trade_index < static_cast<std::size_t>(trade_count()))
                        setup_expect("FIFO reissue drains old origin",
                                     get_trade(static_cast<int>(fill->first_trade_index))
                                         .entry_incarnation == old_origin.incarnation, true);
                    else setup_expect("FIFO reissue trade exists", false, true);
                } else if (std::holds_alternative<native_order::Market>(fill->request().trigger)) {
                    covered += fill->closed_units;
                }
            }
            setup_expect("seed add really opened", openings, 1);
            setup_expect("fresh seed leg really filled", priced, 1);
            // These are behavioral assertions, not setup preconditions: a
            // missing cover must remain visible in the later economic oracle.
            expect("ungated native cover units", covered, 1.0);
            expect("position after ungated cover", signed_position_size(), 0.0);
            timed_events_from = cursor();
            strategy_entry(kTimedAdd, true, kNaN, kNaN, 2.0);
            for (std::size_t i = 0; i < kTimedExits.size(); ++i) {
                strategy_exit(kTimedExits[i], kTimedAdd, 3670.0 + 10.0 * i,
                              kTimedStop, kNaN, kNaN, kNaN, 100.0, {}, 1.0);
                ++timed_exit_calls;
            }
        }
        if (index == kTimedEntryBar) {
            int openings = 0;
            for (const auto& row : native_events(timed_events_from)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (!fill || fill->request().label != kTimedAdd
                    || !(fill->opened_units > 0.0)) continue;
                ++openings;
                timed_origin = fill->handle();
                setup_expect("timed native opening quantity", fill->opened_units, 2.0);
                setup_expect("timed native entry bar", fill->interval_index(), kTimedEntryBar);
                setup_expect("timed native entry time", fill->interval_open_ms(), kTimedEntryMs);
                price("timed native entry price", fill->resolved_price, 3650.585, true);
            }
            setup_expect("one timed native opening", openings, 1);
            setup_expect("two exits declared with entry", timed_exit_calls, 2);
            int lots = 0;
            for (const auto& lot : native_open_lots(kNaN)) {
                if (lot.entry_incarnation != timed_origin.incarnation) continue;
                ++lots;
                setup_expect("timed physical lot units", lot.signed_units, 2.0);
                setup_expect("timed physical lot entry time", lot.entry_time_ms, kTimedEntryMs);
                price("timed physical lot entry price", lot.entry_price, 3650.585, true);
            }
            setup_expect("one timed physical lot", lots, 1);
            setup_complete = setup_ok && timed_origin.incarnation != 0;
        }
        if (index >= kTimedEntryBar && index < kTimedExitBar) {
            int early = 0;
            for (const auto& row : native_events(timed_events_from)) {
                if (!row.command) continue;
                const auto* fill =
                    std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (fill && (fill->request().label == kTimedExits[0]
                             || fill->request().label == kTimedExits[1])) ++early;
            }
            expect("no exit before first stop crossing", early, 0);
        }
        if (index == kTimedExitBar || index == kTimedExitBar + 1) {
            expect("position after required exit", signed_position_size(), 0.0);
            expect("native lots after required exit", native_open_lots(kNaN).size(), 0);
        }
        // A wrong held unit gets a real later market trade in the raw output.
        if (index == kTimedExitBar + 1) strategy_close_all();
    }

    double final_size() const { return signed_position_size(); }
    bool ok = true, setup_ok = true, setup_complete = false;
    int timed_exit_calls = 0;
    native_order::RequestHandle old_origin{}, seed_add_origin{}, timed_origin{};
    std::uint64_t prefix_events_from = 0, timed_events_from = 0;

private:
    void seed_exit() {
        strategy_exit(kCoverExit, kCoverSeed, kCoverLimit, kNaN,
                      kNaN, kNaN, kNaN, 100.0, {}, 1.0);
    }
    std::uint64_t cursor() const {
        std::uint64_t result = 0;
        for (const auto& row : native_events(0))
            if (row.ordinal > result) result = row.ordinal;
        return result;
    }
    std::int64_t cycle_ = 0;
};

struct AdmissionPlacementRows {
    friend auto admission_placement_rows(AdmissionPlacementRows);
};
template<auto Member> struct AdmissionPlacementAccess {
    friend auto admission_placement_rows(AdmissionPlacementRows) { return Member; }
};
template struct AdmissionPlacementAccess<&source::PineExecutionAdapter::placement_>;

void check_admission_fact_roundtrip(CoverTimedAddProbe& probe) {
    namespace sd = source::detail;
    namespace scratch = pineforge::detail;
    using Status = scratch::ScratchStatus;
    try {
        auto require = [&](const char* field, Status status) {
            probe.expect(field, status == Status::ok, true);
            if (status != Status::ok) throw std::runtime_error(field);
        };
        auto version_backing = sd::make_command_scratch_backing(512);
        sd::ExitScratchOrderStats order;
        scratch::CommandScratchStore version_store{
            {2, 512, 1, 1, sd::exit_scratch_key_order(&order),
             scratch::ScratchOpenMode::create_empty}, version_backing.get()};
        require("roundtrip production store", version_store.status());
        sd::ExitScratchVersionsProduction versions(version_store);
        auto payload_backing = sd::make_command_scratch_backing(512);
        scratch::CommandScratchStore payload_store{
            {2, 512, 1, 1, sd::exit_payload_key_order(),
             scratch::ScratchOpenMode::create_empty}, payload_backing.get()};
        require("roundtrip logical store", payload_store.status());
        sd::ExitScratchContext context{payload_store, {"admission-roundtrip", 1}};

        source::PineExecutionAdapter adapter;
        auto& placements = adapter.*admission_placement_rows(AdmissionPlacementRows{});
        source::PlacementSnapshot snapshot;
        snapshot.family = source::PineOrderFamily::ExitLimit;
        snapshot.source_id = "exit";
        snapshot.from_entry = "entry";
        snapshot.requested_qty = 1.0;
        snapshot.command_sequence = 17;
        placements.try_emplace(1, snapshot);
        auto& live = placements.at(1);
        const auto hash = [&] {
            BrokerStateHashSink sink;
            adapter.hash_state(sink);
            return sink.h;
        };

        // Freeze both values, mutate the caller, then restore through each
        // production readback. Hash equality must follow the restored fact.
        for (const bool expected : {true, false}) {
            const std::uint64_t scope = expected ? 1 : 2;
            live.fresh_origin_bypass = expected;
            const auto frozen_hash = hash();
            native_order::Request request;
            request.intent = native_order::HostSized{native_order::HostSizedKind::Close,
                                                     std::nullopt};
            request.label = "exit";
            request.trigger = native_order::Limit{108.0};
            std::string key = "exit-key";
            const auto version = versions.placement(scope, live);
            const auto pending_version = versions.pending(scope, request, live, key);
            scratch::ScratchHandle logical_version = 0, logical_pending = 0;
            require("roundtrip freeze placement",
                    sd::freeze_placement(context, scope, live, logical_version));
            require("roundtrip freeze pending",
                    sd::freeze_pending(context, scope, request, live,
                                       sd::exit_scratch_text(key), logical_pending));
            live.fresh_origin_bypass = !expected;
            request = {};
            key.clear();
            probe.expect("admission fact changes actual adapter hash",
                         hash() != frozen_hash, true);
            const auto restored = versions.placement(version);
            probe.expect("production placement restores admission fact",
                         restored.fresh_origin_bypass, expected);
            live = restored;
            probe.expect("placement restore reproduces actual adapter hash",
                         hash() == frozen_hash, true);

            source::PlacementSnapshot pending;
            pending.fresh_origin_bypass = !expected;
            static_cast<const sd::ExitScratchVersionsProduction&>(versions)
                .pending(pending_version, request, pending, key);
            probe.expect("production pending restores admission fact",
                         pending.fresh_origin_bypass, expected);
            probe.expect("production pending restores request", request.label == "exit", true);
            probe.expect("production pending restores key", key == "exit-key", true);
            live.fresh_origin_bypass = !expected;
            live = pending;
            probe.expect("pending restore reproduces actual adapter hash",
                         hash() == frozen_hash, true);

            source::PlacementReadback pending_view;
            require("roundtrip logical pending placement",
                    source::PendingReadback(payload_store, logical_pending).placement(pending_view));
            for (const auto& view : {
                    source::PlacementReadback(payload_store, logical_version), pending_view}) {
                scratch::PayloadField field;
                require("roundtrip admission field exists",
                        view.find("fresh_origin_bypass", field));
                bool actual = !expected;
                require("roundtrip admission field is boolean", field.boolean_value(actual));
                probe.expect("logical readback restores admission fact", actual, expected);
            }
            // An otherwise complete payload with an old header isolates
            // version rejection from missing/truncated-field failures.
            scratch::ScratchRecordInfo info{};
            require("roundtrip record info", version_store.point(version, info));
            std::vector<std::byte> saved_key(static_cast<std::size_t>(info.key_bytes));
            std::vector<std::byte> old_header(static_cast<std::size_t>(info.value_bytes));
            require("roundtrip record key", version_store.read(
                version, scratch::ScratchField::key, 0, {saved_key.data(), saved_key.size()}));
            require("roundtrip record value", version_store.read(
                version, scratch::ScratchField::value, 0, {old_header.data(), old_header.size()}));
            if (old_header.size() < sizeof(std::uint64_t))
                throw std::runtime_error("missing positional schema header");
            old_header[0] = std::byte{1};
            for (std::size_t i = 1; i < sizeof(std::uint64_t); ++i)
                old_header[i] = std::byte{0};
            require("roundtrip old-header transaction", version_store.begin());
            scratch::ScratchHandle incompatible = 0;
            require("roundtrip old-header record", version_store.put(
                scratch::ScratchSpanSource({saved_key.data(), saved_key.size()}),
                scratch::ScratchSpanSource({old_header.data(), old_header.size()}), incompatible));
            require("roundtrip old-header commit",
                    version_store.commit(scratch::ScratchDurability::buffered));
            bool rejected = false;
            try {
                (void)versions.placement(incompatible);
            } catch (const sd::ExitScratchError& error) {
                rejected = error.status() == Status::corrupt;
            }
            probe.expect("old positional schema is rejected", rejected, true);
            versions.release_scope(scope);
            versions.check_teardown_failure();
            require("roundtrip release logical payload", sd::release_payload_scope(context, scope));
        }
        probe.expect("roundtrip version scopes released", version_store.record_count(), 0);
        probe.expect("roundtrip logical scopes released", payload_store.record_count(), 0);
    } catch (const std::exception& error) {
        std::printf("FAIL %s: admission roundtrip: %s\n", kCoverTimedRow.name, error.what());
        probe.expect("admission roundtrip completed", false, true);
    }
}

bool run_cover_timed_add() {
    CoverTimedAddProbe probe;
    check_admission_fact_roundtrip(probe);
    const std::array<std::array<double, 4>, 12> prices{{
        {3650, 3651, 3649, 3650},          // 0: synthetic seed call, 12:45
        {3650, 3651, 3649, 3650},          // 1: seed opens, limit call
        {3655, 3661, 3654, 3660},          // 2: seed's first one-unit limit
        {3655, 3656, 3654, 3655},          // 3: add and full reissue together
        {3650, 3661, 3649, 3652.2},        // 4: FIFO fill + cover, timed calls
        {3650.585, 3653.47, 3649.475, 3652.205}, // 5: 14:00 timed entry
        {3652.2, 3654.495, 3651.64, 3652.835},  // 6: 14:15, no stop touch
        {3652.89, 3653.62, 3648.67, 3649.88},   // 7: 14:30, no stop touch
        {3649.86, 3650.1, 3647.425, 3648.06},   // 8: 14:45, stop first crossed
        {3648.05, 3648.32, 3641.79, 3643.98},   // 9: 15:00, cleanup call
        {3644, 3645, 3643, 3644},          // 10: synthetic cleanup fill
        {3644, 3645, 3643, 3644},          // 11: synthetic trailing bar
    }};
    std::array<Bar, prices.size()> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto& p = prices[i];
        bars[i] = {p[0], p[1], p[2], p[3], 1000,
                   kTimedEntryMs + (static_cast<std::int64_t>(i) - kTimedEntryBar) * kInterval};
    }
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    probe.setup_expect("run succeeded", probe.last_error().empty(), true);
    for (const auto& row : probe.native_events(0)) {
        if (!row.command) continue;
        const auto* fill =
            std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
        if (!fill) continue;
        const auto& trigger = fill->request().trigger;
        const char* kind = std::holds_alternative<native_order::Market>(trigger) ? "market"
            : (std::holds_alternative<native_order::Limit>(trigger) ? "limit" : "stop");
        std::printf("%s native %s %s handle=%llu bar=%d time=%lld opened=%.17g "
                    "closed=%.17g price=%.17g\n", kCoverTimedRow.name,
                    fill->request().label.c_str(), kind,
                    static_cast<unsigned long long>(fill->handle().incarnation),
                    fill->interval_index(), static_cast<long long>(fill->interval_open_ms()),
                    fill->opened_units, fill->closed_units, fill->resolved_price);
    }
    for (int i = 0; i < probe.trade_count(); ++i) {
        const auto& trade = probe.get_trade(i);
        std::printf("%s trade entry=%s origin=%llu exit=%s qty=%.17g entry=%lld/%.17g "
                    "exit=%lld/%.17g bars=%d/%d open=%d\n", kCoverTimedRow.name,
                    trade.entry_id.c_str(), static_cast<unsigned long long>(trade.entry_incarnation),
                    trade.exit_id.c_str(), trade.qty, static_cast<long long>(trade.entry_time),
                    trade.entry_price, static_cast<long long>(trade.exit_time), trade.exit_price,
                    trade.entry_bar_index, trade.exit_bar_index, trade.open_at_end ? 1 : 0);
    }
    if (!probe.setup_ok || !probe.setup_complete) {
        std::printf("INCOMPLETE %s: native setup failed; no economic RED established\n",
                    kCoverTimedRow.name);
        return false;
    }

    int cover_requests = 0, cover_fills = 0, seed_limits = 0;
    std::array<int, 2> limits{}, stops{}, fills{}, trades{};
    std::array<native_order::RequestHandle, 2> stop_handles{};
    for (const auto& row : probe.native_events(probe.prefix_events_from)) {
        if (!row.command) continue;
        if (const auto* accepted =
                std::get_if<native_order::AcceptedEvent>(&*row.command)) {
            const auto& request = accepted->request();
            if (request.label == kCoverExit
                && std::holds_alternative<native_order::Market>(request.trigger)) {
                ++cover_requests;
                const auto* reduce = std::get_if<native_order::Reduce>(&request.intent);
                probe.expect("native cover is a reduction", reduce != nullptr, true);
                if (reduce) {
                    const auto* units = std::get_if<native_order::ExplicitUnits>(&reduce->size);
                    probe.expect("native cover uses explicit units", units != nullptr, true);
                    if (units) probe.expect("native cover request units", units->units, 1.0);
                }
                probe.expect("native cover binds openings",
                             std::holds_alternative<native_order::BindOpenings>(request.owner), true);
            }
            for (std::size_t i = 0; i < kTimedExits.size(); ++i) {
                if (request.label != kTimedExits[i]) continue;
                probe.expect("timed native FIFO owner",
                             std::holds_alternative<native_order::Independent>(request.owner), true);
                const auto* intent = std::get_if<native_order::HostSized>(&request.intent);
                probe.expect("timed native host-sized request", intent != nullptr, true);
                if (intent)
                    probe.expect("timed native close intent",
                                 intent->kind == native_order::HostSizedKind::Close, true);
                if (const auto* limit = std::get_if<native_order::Limit>(&request.trigger)) {
                    ++limits[i];
                    probe.price("timed native limit threshold", limit->price,
                                3670.0 + 10.0 * i - kTimedTick / 2.0);
                }
                if (const auto* stop = std::get_if<native_order::Stop>(&request.trigger)) {
                    ++stops[i];
                    stop_handles[i] = accepted->handle();
                    probe.price("timed native stop threshold", stop->price,
                                std::nextafter(kTimedStop + kTimedTick / 2.0,
                                               -std::numeric_limits<double>::infinity()));
                }
            }
        }
        if (const auto* fill =
                std::get_if<native_order::ExecutionAppliedEvent>(&*row.command)) {
            if (fill->request().label == kCoverExit) {
                if (std::holds_alternative<native_order::Market>(fill->request().trigger)) {
                    ++cover_fills;
                    probe.expect("native cover fill units", fill->closed_units, 1.0);
                    probe.expect("native cover opens nothing", fill->opened_units, 0.0);
                    probe.expect("native cover fill bar", fill->interval_index(), 4);
                    probe.expect("native cover fill time", fill->interval_open_ms(),
                                 kTimedEntryMs - kInterval);
                    probe.price("native cover fill price", fill->resolved_price, kCoverLimit);
                    probe.expect("native cover closes one trade", fill->closed_trade_count, 1);
                    if (fill->first_trade_index < static_cast<std::size_t>(probe.trade_count()))
                        probe.expect("native cover closes seed add",
                                     probe.get_trade(static_cast<int>(fill->first_trade_index))
                                         .entry_incarnation == probe.seed_add_origin.incarnation, true);
                    else probe.expect("native cover trade exists", false, true);
                } else if (std::holds_alternative<native_order::Limit>(fill->request().trigger)) {
                    ++seed_limits;
                }
            }
            for (std::size_t i = 0; i < kTimedExits.size(); ++i) {
                if (fill->request().label != kTimedExits[i]) continue;
                ++fills[i];
                probe.expect("timed fill is accepted stop", fill->handle() == stop_handles[i], true);
                probe.expect("timed native closed quantity", fill->closed_units, 1.0);
                probe.expect("timed native opened quantity", fill->opened_units, 0.0);
                probe.expect("timed native exit bar", fill->interval_index(), kTimedExitBar);
                probe.expect("timed native exit time", fill->interval_open_ms(), kTimedExitMs);
                probe.price("timed native exit price", fill->resolved_price, kTimedStop);
                probe.expect("timed native fill closes one trade", fill->closed_trade_count, 1);
                if (fill->first_trade_index < static_cast<std::size_t>(probe.trade_count()))
                    probe.expect("timed native fill owns timed Add",
                                 probe.get_trade(static_cast<int>(fill->first_trade_index))
                                     .entry_incarnation == probe.timed_origin.incarnation, true);
                else probe.expect("timed native trade exists", false, true);
            }
        }
    }
    probe.expect("one native cover request", cover_requests, 1);
    probe.expect("one native cover fill", cover_fills, 1);
    probe.expect("one reissued seed limit, never an old-origin refire", seed_limits, 1);

    int timed_trades = 0;
    double required_units = 0.0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const auto& trade = probe.get_trade(i);
        if (trade.entry_incarnation != probe.timed_origin.incarnation) continue;
        ++timed_trades;
        bool named_exit = false;
        for (std::size_t j = 0; j < kTimedExits.size(); ++j)
            if (trade.exit_id == kTimedExits[j]) { ++trades[j]; named_exit = true; }
        probe.expect("timed trade entry id", trade.entry_id == kTimedAdd, true);
        probe.expect("timed trade exit id", named_exit, true);
        probe.expect("timed trade entry bar", trade.entry_bar_index, kTimedEntryBar);
        probe.expect("timed trade entry time", trade.entry_time, kTimedEntryMs);
        probe.price("timed trade entry price", trade.entry_price, 3650.585);
        probe.expect("timed trade exit bar", trade.exit_bar_index, kTimedExitBar);
        probe.expect("timed trade exit time", trade.exit_time, kTimedExitMs);
        probe.price("timed trade exit price", trade.exit_price, 3647.85);
        probe.expect("timed trade quantity", trade.qty, 1.0);
        probe.expect("timed trade long", trade.is_long, true);
        probe.expect("timed trade closed", trade.open_at_end, false);
        if (trade.exit_time == kTimedExitMs && named_exit && !trade.open_at_end)
            required_units += trade.qty;
    }
    for (std::size_t i = 0; i < kTimedExits.size(); ++i) {
        const std::string prefix = std::string(kTimedExits[i]) + " ";
        probe.expect(prefix + "native limits", limits[i], 1);
        probe.expect(prefix + "native stops", stops[i], 1);
        probe.expect(prefix + "native fills", fills[i], 1);
        probe.expect(prefix + "exact economic rows", trades[i], 1);
    }
    probe.expect("two timed one-unit trades", timed_trades, 2);
    probe.expect("units closed at required time", required_units, 2.0);
    probe.expect("all trades", probe.trade_count(), 5);
    probe.expect("final position", probe.final_size(), 0.0);
    if (probe.ok) std::printf("PASS %s\n", kCoverTimedRow.name);
    return probe.ok;
}

struct CoverGridRow {
    Row row;
    bool any;
    double grid;
};

constexpr CoverGridRow kCoverGridRows[] = {
    {{"R-cover-long-FIFO-absent", true, false, false}, false, 0.0},
    {{"R-cover-long-FIFO-grid", true, false, false}, false, 0.00001},
    {{"R-cover-long-ANY-absent", true, false, false}, true, 0.0},
    {{"R-cover-long-ANY-grid", true, false, false}, true, 0.00001},
    {{"R-cover-short-FIFO-absent", false, false, false}, false, 0.0},
    {{"R-cover-short-FIFO-grid", false, false, false}, false, 0.00001},
    {{"R-cover-short-ANY-absent", false, false, false}, true, 0.0},
    {{"R-cover-short-ANY-grid", false, false, false}, true, 0.00001},
};

// The timed row above keeps its original inputs and oracles. These cells
// exercise its causal prefix on both native quantity-grid configurations.
// FIFO drains the older opening and covers the add; ANY drains its bound add
// and leaves the older remnant for the later explicit close.
class CoverGridProbe final : public source::PineStrategyHost {
public:
    explicit CoverGridProbe(const CoverGridRow& row) : cell(row) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.pyramiding = 2;
        cfg.close_entries_rule_any = cell.any;
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 0.02;
        cfg.margin_long = cfg.margin_short = 100.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", cell.grid);
        fixture_retain_all_events();
    }
    using source::PineStrategyHost::native_state;

    void expect(const std::string& field, double actual, double expected) {
        if (!equal_number(cell.row, field, actual, expected)) ok = false;
    }
    void quantity(const std::string& field, double actual, double expected) {
        if (std::isfinite(actual) && std::abs(actual - expected) <= 1e-12) return;
        expect(field, actual, expected);
    }
    void setup(const std::string& field, bool condition) {
        if (!equal_number(cell.row, "setup " + field, condition, true)) setup_ok = false;
    }
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        const double sign = cell.row.is_long ? 1.0 : -1.0;
        if (bar == 0) strategy_entry("seed", cell.row.is_long, kNaN, kNaN, 0.02);
        if (bar == 1) {
            setup("initial position", std::abs(signed_position_size() - sign * 0.02) < 1e-12);
            for (const auto& row : native_events(0)) {
                if (!row.command) continue;
                const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
                if (!fill || fill->request().label != "seed" || !(std::abs(fill->opened_units) > 0.0))
                    continue;
                old_origin = fill->handle();
                cycle = fill->cycle_after;
                setup("initial opening units", std::abs(std::abs(fill->opened_units) - 0.02) < 1e-12);
                setup("initial opening bar", fill->interval_index() == 1);
            }
            setup("initial opening provenance", old_origin.incarnation != 0);
            arm();
        }
        if (bar == 3) {
            setup("one initial partial trade", trade_count() == 1);
            setup("full reissue basis", std::abs(signed_position_size() - sign * 0.01) < 1e-12);
            if (trade_count() == 1) {
                const auto& trade = get_trade(0);
                setup("initial trade provenance", trade.entry_incarnation == old_origin.incarnation);
                setup("initial trade quantity", std::abs(trade.qty - 0.01) < 1e-12);
                setup("initial trade bar", trade.exit_bar_index == 2);
                setup("initial trade price", trade.exit_price == price_for(cell.row, 110.0));
            }
            const auto consumed = adapter_.fixture_consumed_partial_exit_cycles().lookup(
                std::string("slice") + '\x1f' + "seed");
            setup("consumed family in current cycle", cycle > 0 && consumed && *consumed == cycle);
            prefix_events_from = cursor();
            strategy_entry("seed", cell.row.is_long, kNaN, kNaN, 0.01);
            // This quantity covers all held units at the command boundary,
            // so it must retain ordinary same-id cover eligibility.
            arm();
            for (const auto& row : native_events(prefix_events_from)) {
                if (!row.command) continue;
                const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*row.command);
                if (accepted && accepted->request().label == "seed") add_origin = accepted->handle();
            }
            setup("distinct pending add", add_origin.incarnation != 0 && add_origin != old_origin);
            int legs = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                if (row.snapshot.source_id != "slice") continue;
                ++legs;
                setup("leg binds add", row.snapshot.bracket_origin == add_origin);
                setup("ordinary admission", !row.snapshot.fresh_origin_bypass);
                setup("explicit reservation", std::abs(row.snapshot.requested_qty - 0.01) < 1e-12
                    && std::abs(row.snapshot.projection_remaining_qty - 0.01) < 1e-12);
            }
            setup("one new leg, no old-origin revival", legs == 1);
            prefix_ready = true;
        }
        if (bar == 4)
            quantity("position after priced fill and cover", signed_position_size(),
                     cell.any ? sign * 0.01 : 0.0);
        if (bar == 5) strategy_close_all();
    }
    double final_size() const { return signed_position_size(); }

    const CoverGridRow& cell;
    bool ok = true, setup_ok = true, prefix_ready = false;
    native_order::RequestHandle old_origin{}, add_origin{};
    std::uint64_t prefix_events_from = 0;
    std::int64_t cycle = 0;

private:
    void arm() {
        strategy_exit("slice", "seed", price_for(cell.row, 110.0), kNaN,
                      kNaN, kNaN, kNaN, 100.0, {}, 0.01);
    }
    std::uint64_t cursor() const {
        std::uint64_t ordinal = 0;
        for (const auto& row : native_events(0))
            if (row.ordinal > ordinal) ordinal = row.ordinal;
        return ordinal;
    }
};

bool run_cover_grid(const CoverGridRow& cell) {
    CoverGridProbe probe(cell);
    std::array<Bar, 8> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double high = i == 2 || i == 4 ? 111.0 : 101.0;
        bars[i] = {100.0,
                   price_for(cell.row, cell.row.is_long ? high : 99.0),
                   price_for(cell.row, cell.row.is_long ? 99.0 : high),
                   100.0, 1000.0, kStart + static_cast<std::int64_t>(i) * kInterval};
    }
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    probe.setup("run succeeded", probe.last_error().empty());
    probe.setup("prefix executed", probe.prefix_ready);
    const auto state = probe.native_state();
    probe.setup("native spec exists", state.spec != nullptr);
    if (state.spec) {
        probe.setup("actual native grid presence",
                    state.spec->quantity_grid.has_value() == (cell.grid > 0.0));
        if (cell.grid > 0.0)
            probe.setup("actual native grid value", state.spec->quantity_grid
                && std::abs(*state.spec->quantity_grid - cell.grid) < 1e-15);
    }
    if (!probe.setup_ok) {
        std::printf("INCOMPLETE %s: native setup failed\n", cell.row.name);
        return false;
    }
    int add_fills = 0, priced_requests = 0, priced_fills = 0;
    int cover_requests = 0, cover_fills = 0;
    for (const auto& row : probe.native_events(probe.prefix_events_from)) {
        if (!row.command) continue;
        if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*row.command)) {
            const auto& request = accepted->request();
            if (request.label != "slice") continue;
            if (std::holds_alternative<native_order::Limit>(request.trigger)) {
                ++priced_requests;
                if (cell.any) {
                    const auto* owner = std::get_if<native_order::BindOpening>(&request.owner);
                    probe.expect("ANY native owner binds add",
                                 owner && owner->opening == probe.add_origin, true);
                } else {
                    probe.expect("FIFO native book owner",
                                 std::holds_alternative<native_order::Independent>(request.owner), true);
                }
            } else if (std::holds_alternative<native_order::Market>(request.trigger)) {
                ++cover_requests;
                const auto* reduce = std::get_if<native_order::Reduce>(&request.intent);
                const auto* units = reduce
                    ? std::get_if<native_order::ExplicitUnits>(&reduce->size) : nullptr;
                probe.expect("cover uses native explicit reduction", units != nullptr, true);
                if (units) probe.quantity("cover request units", units->units, 0.01);
                probe.expect("cover binds openings",
                             std::holds_alternative<native_order::BindOpenings>(request.owner), true);
            } else {
                probe.expect("unexpected slice request", true, false);
            }
        }
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*row.command);
        if (!fill) continue;
        if (fill->handle() == probe.add_origin && std::abs(fill->opened_units) > 0.0) {
            ++add_fills;
            probe.quantity("add opened units", std::abs(fill->opened_units), 0.01);
            probe.expect("add opening bar", fill->interval_index(), 4);
            probe.expect("add cycle", fill->cycle_after, probe.cycle);
        }
        if (fill->request().label != "slice") continue;
        const bool cover = std::holds_alternative<native_order::Market>(fill->request().trigger);
        if (cover) ++cover_fills;
        else {
            ++priced_fills;
            probe.expect("priced fill is native limit",
                         std::holds_alternative<native_order::Limit>(fill->request().trigger), true);
        }
        probe.quantity("native closed quantity", fill->closed_units, 0.01);
        probe.quantity("native opened quantity", fill->opened_units, 0.0);
        probe.expect("native fill bar", fill->interval_index(), 4);
        probe.expect("native fill price", fill->resolved_price, price_for(cell.row, 110.0));
        probe.expect("one native closed trade", fill->closed_trade_count, 1);
        const bool exists = fill->first_trade_index < static_cast<std::size_t>(probe.trade_count());
        probe.expect("native closed trade exists", exists, true);
        if (exists) {
            const auto& trade = probe.get_trade(static_cast<int>(fill->first_trade_index));
            const auto origin = cover || cell.any ? probe.add_origin : probe.old_origin;
            probe.expect("native fill closes required opening",
                         trade.entry_incarnation == origin.incarnation, true);
        }
    }
    probe.expect("one actual add opening", add_fills, 1);
    probe.expect("one reissued priced request", priced_requests, 1);
    probe.expect("one reissued priced fill", priced_fills, 1);
    probe.expect("native cover request count", cover_requests, cell.any ? 0 : 1);
    probe.expect("native cover fill count", cover_fills, cell.any ? 0 : 1);
    probe.expect("closed trade count", probe.trade_count(), 3);
    double closed = 0.0;
    for (int i = 0; i < probe.trade_count(); ++i) {
        const auto& trade = probe.get_trade(i);
        probe.expect("trade entry id", trade.entry_id == "seed", true);
        probe.expect("trade direction", trade.is_long, cell.row.is_long);
        probe.expect("trade is closed", trade.open_at_end, false);
        probe.quantity("trade quantity", trade.qty, 0.01);
        closed += trade.qty;
    }
    probe.quantity("all opened units closed", closed, 0.03);
    probe.quantity("final position", probe.final_size(), 0.0);
    if (probe.ok) std::printf("PASS %s\n", cell.row.name);
    return probe.ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::printf("Expected zero arguments or one row name.\n");
        return 1;
    }
    int selected = 0;
    int failures = 0;
    for (const Row& row : kRows) {
        if (argc == 2 && std::string(argv[1]) != row.name) continue;
        ++selected;
        if (!run_row(row)) ++failures;
    }
    if (argc == 1 || std::string(argv[1]) == kQueuedRow) {
        ++selected;
        if (!run_queued_predecessor()) ++failures;
    }
    for (const Row& row : kFreshRows) {
        if (argc == 2 && std::string(argv[1]) != row.name) continue;
        ++selected;
        if (!run_fresh_add(row)) ++failures;
    }
    for (const Row& row : kSettledRows) {
        if (argc == 2 && std::string(argv[1]) != row.name) continue;
        ++selected;
        if (!run_settled_add(row)) ++failures;
    }
    for (const CancelAddRow& row : kCancelAddRows) {
        if (argc == 2 && std::string(argv[1]) != row.name) continue;
        ++selected;
        if (!run_cancel_add(row)) ++failures;
    }
    if (argc == 1 || std::string(argv[1]) == kCoverTimedRow.name) {
        ++selected;
        if (!run_cover_timed_add()) ++failures;
    }
    for (const CoverGridRow& cell : kCoverGridRows) {
        if (argc == 2 && std::string(argv[1]) != "cover-grid"
            && std::string(argv[1]) != cell.row.name) continue;
        ++selected;
        if (!run_cover_grid(cell)) ++failures;
    }
    if (selected == 0) {
        std::printf("Unknown row: %s\n", argv[1]);
        return 1;
    }
    std::printf("%d rows, %d failures\n", selected, failures);
    return failures == 0 ? 0 : 1;
}
