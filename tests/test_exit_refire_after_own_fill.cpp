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
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace pineforge;

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

// Extrapolation, not measured: the zero-capacity reissue must remove its
// queued predecessor (src/source/pine_adapter.cpp:12322, exit()'s
// clear_existing_exit_order contract; :15261, pending_bracket_legs_ erase).
// This case regresses the inherited LIVE-ONLY partial candidate. The original
// 05552883 parent unconditionally cancels here and is expected to pass this
// case; its release-refire failures are the separate four rows above.
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
            native_order::RunIdentity filled_run{};
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
                    filled_run = fill->handle().run;
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
            const auto queued = observe("queued stop", kConsumedExit, true, 2.0, 1.0,
                                        source::PineOrderFamily::ExitStop, kQueuedStop);
            expect("queued stop has no predecessor",
                   queued.snapshot.projection_predecessor, 0);
            expect("queued stop has no queue rank", queued.snapshot.chain_origin_sequence, 0);
            expect("queued stop has no source sequence", queued.snapshot.source_sequence, 0);
            expect("queued stop is not native", working(kConsumedExit), 0);
            holder(2.0);
            const auto expanded = observe("expanded holder", kHolderExit, false, 2.0, 2.0,
                                          source::PineOrderFamily::ExitStop, kHolderStop);
            expect("holder retains an earlier queue rank",
                   expanded.snapshot.chain_origin_sequence > 0, true);
            expect("expanded holder is native", working(kHolderExit), 1);
            observe("stop still queued before refusal", kConsumedExit, true, 2.0, 1.0,
                    source::PineOrderFamily::ExitStop, kQueuedStop);
            expect("position unchanged before refusal", signed_position_size(), 2.0);

            std::uint64_t cursor = 0;
            for (const auto& row : native_events(0))
                if (row.ordinal > cursor) cursor = row.ordinal;
            stop();
            // First cancellation assertion: the live-only partial leaves one.
            expect("queued stop after failed reservation", pending(kConsumedExit), 0);
            expect("failed replacement is not native", working(kConsumedExit), 0);
            int cancelled_members = 0;
            for (const auto& row : native_events(cursor)) {
                if (!row.command) continue;
                if (const auto* event =
                        std::get_if<native_order::NotWorkingEvent>(&*row.command)) {
                    ++cancelled_members;
                    expect("cancel retains filled member run",
                           event->target.run == filled_run, true);
                    expect("cancel targets non-working sentinel",
                           event->target.incarnation
                               == std::numeric_limits<std::uint64_t>::max(), true);
                }
            }
            expect("filled family cancellation outcome", cancelled_members, 1);
            observe("holder after refusal", kHolderExit, false, 2.0, 2.0,
                    source::PineOrderFamily::ExitStop, kHolderStop);
            sequence_seen = true;
        }
        if (index == 4) {
            stop_touch_seen = bar.low < kQueuedStop && bar.high > kQueuedStop
                && bar.low > kHolderStop;
            expect("later bar crosses only consumed stop", stop_touch_seen, true);
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
        {105, 106, 104, 105, 1000, kStart + 3 * kInterval},   // 3: queue sequence
        {100, 101, 89, 95, 1000, kStart + 4 * kInterval},     // 4: leaked stop
        {100, 102, 98, 100, 1000, kStart + 5 * kInterval},    // 5: final close call
        {101, 103, 99, 102, 1000, kStart + 6 * kInterval},    // 6: final close
        {102, 103, 100, 101, 1000, kStart + 7 * kInterval},   // 7: trailing bar
    }};
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    probe.expect("run succeeded", probe.last_error().empty(), true);
    probe.expect("opening setup observed", probe.opening_seen, true);
    probe.expect("coexistence sequence observed", probe.sequence_seen, true);
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
// exit() submits thresholds (src/source/pine_adapter.cpp:13300 and :13326)
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
            // First behavioural RED on c7: its cycle-wide consumed return
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
// This covers the missing lifetime shape, not a replay of the private tape.
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
    if (selected == 0) {
        std::printf("Unknown row: %s\n", argv[1]);
        return 1;
    }
    std::printf("%d rows, %d failures\n", selected, failures);
    return failures == 0 ? 0 : 1;
}
