// EN-134: full runs through the public C ABI, including report production.
// Written before the guards. Fault rows must answer PF_NATIVE_E_RUN_FAILED,
// engine_invariant and zero report trades. On the base they complete and
// carry the supplied non-finite excursion (or overflowed pnl_pct).
//
// Closing runs open TWO independent lots: a one-lot full close takes the
// fused settlement, which bypasses prepare_native_settlement_commit. That
// separate gap is reported in the lane handoff, not changed here.
//
// No private headers, engine casts, direct guard calls or book injection.
// --controls emits every scalar report field and its array payloads as
// explicit binary64/integer words for a same-host base/head comparison.
// Round-b fixtures below add one-lot and adapter witnesses. The two-lot
// round-a fixtures and the historical description above are retained intact.
#include <pineforge/native_c_api.h>
#include <pineforge/pineforge.h>

// Round b compiles this same TU twice. Only the source-layer target supplies
// this public header through its compile definition; the native target keeps
// its source-free include closure and all its kernel-profile registrations.
#ifdef PINEFORGE_SETTLEMENT_SOURCE_HEADER
#include PINEFORGE_SETTLEMENT_SOURCE_HEADER
#include <pineforge/selected_window.h>
#include <pineforge/metrics.hpp>
#include "report_capture_fixture.hpp"
#include "report_failure_checks.hpp"
#endif

#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;
const char* current_row = "setup";

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,     \
                         current_row, #condition);                              \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

constexpr int kBars = 5;
constexpr std::int64_t kStart = 1704067200000LL;
constexpr std::int64_t kInterval = 300000;

struct Row {
    const char* name;
    bool range_end;
    bool owns_excursions;
    double runup;
    double drawdown;
    bool percentage_overflow = false;
    bool flat_tape = false;

    bool expects_failure() const {
        return percentage_overflow
            || (owns_excursions && (!std::isfinite(runup) || !std::isfinite(drawdown)));
    }
    int lot_count() const { return range_end ? 1 : 2; }
};

std::vector<Row> rows() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    return {
        {"closed_runup_nan", false, true, nan, 3.0},
        {"closed_drawdown_nan", false, true, 7.0, nan},
        {"closed_runup_posinf", false, true, inf, 3.0},
        {"closed_drawdown_posinf", false, true, 7.0, inf},
        {"closed_runup_neginf", false, true, -inf, 3.0},
        {"closed_drawdown_neginf", false, true, 7.0, -inf},
        {"range_runup_nan", true, true, nan, 3.0},
        {"range_drawdown_nan", true, true, 7.0, nan},
        {"range_runup_posinf", true, true, inf, 3.0},
        {"range_drawdown_posinf", true, true, 7.0, inf},
        {"range_runup_neginf", true, true, -inf, 3.0},
        {"range_drawdown_neginf", true, true, 7.0, -inf},
        {"closed_pnl_pct_overflow", false, false, 0.0, 0.0, true},
        {"range_pnl_pct_overflow", true, false, 0.0, 0.0, true},
        {"finite_closed_zero", false, true, 0.0, 0.0},
        {"finite_closed_owned", false, true, 37.5, 12.25},
        {"finite_closed_signed", false, true, -7.5, -3.25},
        {"finite_range_zero", true, true, 0.0, 0.0},
        {"finite_range_owned", true, true, 37.5, 12.25},
        {"kernel_closed_normal", false, false, 0.0, 0.0},
        {"kernel_range_normal", true, false, 0.0, 0.0},
        {"kernel_closed_zero", false, false, 0.0, 0.0, false, true},
        {"kernel_range_zero", true, false, 0.0, 0.0, false, true},
    };
}

std::array<pf_bar_t, kBars> tape(const Row& row) {
    std::array<pf_bar_t, kBars> bars{};
    for (int i = 0; i < kBars; ++i) {
        auto& bar = bars[static_cast<std::size_t>(i)];
        bar.timestamp = kStart + i * kInterval;
        bar.volume = 10.0;
        if (row.percentage_overflow) {
            // Entry on bar 1, close on bar 3 (or mark on bar 4). No grid.
            bar.open = std::ldexp(1.0, i < 3 ? -512 : 512);
            bar.high = bar.low = bar.close = bar.open;
        } else if (row.flat_tape) {
            bar.open = bar.high = bar.low = bar.close = 100.0;
        } else {
            bar.open = 100.0 + i;
            bar.high = bar.open + 2.0;
            bar.low = bar.open - 1.0;
            bar.close = bar.open + 1.0;
        }
    }
    return bars;
}

struct Host {
    const Row& row;
    pf_strategy_t handle = nullptr;
    std::array<std::uint64_t, 2> entries{};
    int calculations = 0;
    int command_error = PF_NATIVE_OK;
    int opened = 0;
    int closed = 0;
    int excursion_calls = 0;
    int bad_answers = 0;
    std::uint64_t live_lots = 0;
    double live_units = 0.0;

    void submit(std::uint32_t intent, const char* label, std::uint64_t* incarnation) {
        pf_native_request_v1 request{};
        request.struct_size = static_cast<std::uint32_t>(sizeof(request));
        request.version = PF_NATIVE_API_VERSION;
        request.intent = intent;
        request.intent_value = intent == PF_NATIVE_INTENT_TRANSACT ? 1.0 : 0.0;
        request.trigger = PF_NATIVE_TRIGGER_MARKET;
        request.label = label;
        const int rc = strategy_native_submit_v1(handle, &request, incarnation, nullptr);
        if (rc != PF_NATIVE_OK && command_error == PF_NATIVE_OK) command_error = rc;
    }
};

int on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& host = *static_cast<Host*>(user);
    ++host.calculations;
    if (host.calculations == 1) {
        host.submit(PF_NATIVE_INTENT_TRANSACT, "row-entry-a", &host.entries[0]);
        if (!host.row.range_end)
            host.submit(PF_NATIVE_INTENT_TRANSACT, "row-entry-b", &host.entries[1]);
    } else if (host.calculations == 2) {
        CHECK(strategy_native_position_v1(host.handle, &host.live_units, nullptr,
                                         &host.live_lots) == PF_NATIVE_OK);
    } else if (host.calculations == 3 && !host.row.range_end) {
        host.submit(PF_NATIVE_INTENT_FLATTEN, "row-exit", nullptr);
    }
    return 0;
}

int on_applied(void* user, const pf_native_applied_v1* applied,
               const pf_native_decision_v1*) {
    auto& host = *static_cast<Host*>(user);
    if (applied->opened_units != 0.0) ++host.opened;
    if (applied->closed_units != 0.0) ++host.closed;
    return 0;
}

int on_lot_excursion(void* user, const pf_native_lot_excursion_v1* facts,
                     double* favorable, double* adverse) {
    auto& host = *static_cast<Host*>(user);
    ++host.excursion_calls;
    CHECK(facts->entry_incarnation != 0);
    CHECK(facts->entry_incarnation == host.entries[0]
          || facts->entry_incarnation == host.entries[1]);
    CHECK(facts->entry_price == 101.0);
    CHECK(facts->lot_qty == 1.0 && facts->closed_qty == 1.0);
    // A range-end host never submits a close: its only excursion consultation
    // is the marking row, at the final close, after all five calculations.
    CHECK(facts->fill_price == (host.row.range_end ? 105.0 : 103.0));
    if (host.row.range_end) CHECK(host.calculations == kBars && host.closed == 0);
    *favorable = host.row.runup;
    *adverse = host.row.drawdown;
    if (!std::isfinite(*favorable) || !std::isfinite(*adverse)) ++host.bad_answers;
    return 1;
}

pf_native_run_spec_v1 spec() {
    pf_native_run_spec_v1 out{};
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.session_key = "settlement-row-finite";
    out.run_number = 1;
    out.input_tf = out.script_tf = "5";
    out.ticker = "ROW";
    out.tickerid = "TEST:ROW";
    out.type = "crypto";
    out.currency = "USDT";
    out.basecurrency = "ETH";
    out.description = out.volumetype = out.chart_timezone = "";
    out.timezone = "UTC";
    out.session = "24x7";
    out.initial_capital = 10000.0;
    out.point_value = out.account_fx = 1.0;
    out.price_tick = 0.0;
    out.fee_kind = PF_NATIVE_FEE_CASH_PER_UNIT;
    out.fee_value = 0.0;
    out.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    return out;
}

// Compare values, never object padding, pointers or a lossy digest. The output
// is suitable for an exact base/head text diff on the same build host.
struct Snapshot {
    std::vector<std::uint64_t> words;
    template<class T> void add(T value) { words.push_back(static_cast<std::uint64_t>(value)); }
    void add(double value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "binary64 report");
        std::memcpy(&bits, &value, sizeof(bits));
        words.push_back(bits);
    }
};

void snapshot_trade(Snapshot& out, const pf_trade_t& t) {
    out.add(t.entry_time); out.add(t.exit_time);
    out.add(t.entry_price); out.add(t.exit_price);
    out.add(t.pnl); out.add(t.pnl_pct); out.add(t.is_long);
    out.add(t.max_runup); out.add(t.max_drawdown);
    out.add(t.qty); out.add(t.commission);
    out.add(t.entry_bar_index); out.add(t.exit_bar_index); out.add(t.open_at_end);
}

void snapshot_stats(Snapshot& out, const pf_trade_stats_t& s) {
    out.add(s.num_trades); out.add(s.num_wins); out.add(s.num_losses); out.add(s.num_even);
    out.add(s.percent_profitable); out.add(s.net_profit); out.add(s.net_profit_pct);
    out.add(s.gross_profit); out.add(s.gross_profit_pct);
    out.add(s.gross_loss); out.add(s.gross_loss_pct); out.add(s.profit_factor);
    out.add(s.avg_trade); out.add(s.avg_trade_pct); out.add(s.avg_win); out.add(s.avg_win_pct);
    out.add(s.avg_loss); out.add(s.avg_loss_pct); out.add(s.ratio_avg_win_avg_loss);
    out.add(s.largest_win); out.add(s.largest_win_pct);
    out.add(s.largest_loss); out.add(s.largest_loss_pct);
    out.add(s.commission_paid); out.add(s.expectancy);
    out.add(s.max_consecutive_wins); out.add(s.max_consecutive_losses);
    out.add(s.avg_bars_in_trade); out.add(s.avg_bars_in_wins); out.add(s.avg_bars_in_losses);
}

void print_snapshot(const Row& row, const pf_report_t& report) {
    Snapshot out;
    out.add(report.total_trades); out.add(report.trades_len); out.add(report.net_profit);
    out.add(report.input_bars_processed); out.add(report.script_bars_processed);
    out.add(report.security_feeds_total); out.add(report.security_complete_total);
    out.add(report.security_partial_total); out.add(report.magnifier_sub_bars_total);
    out.add(report.magnifier_sample_ticks_total);
    out.add(report.input_tf_seconds); out.add(report.script_tf_seconds);
    out.add(report.script_tf_ratio); out.add(report.needs_aggregation);
    out.add(report.bar_magnifier_enabled);
    out.add(report.security_diag_len); out.add(report.trace_len); out.add(report.trace_names_len);
    // This host declares no security sites or traces; their complete payload
    // is empty on both trees. An unexpected payload is a failing control.
    CHECK(report.security_diag_len == 0 && report.trace_len == 0 && report.trace_names_len == 0);
    for (int i = 0; i < report.trades_len; ++i) snapshot_trade(out, report.trades[i]);
    snapshot_stats(out, report.metrics.all);
    snapshot_stats(out, report.metrics.longs);
    snapshot_stats(out, report.metrics.shorts);
    const auto& e = report.metrics.equity;
    out.add(e.max_equity_drawdown); out.add(e.max_equity_drawdown_pct);
    out.add(e.max_equity_runup); out.add(e.max_equity_runup_pct);
    out.add(e.buy_hold_return); out.add(e.buy_hold_return_pct);
    out.add(e.sharpe_monthly); out.add(e.sortino_monthly);
    out.add(e.sharpe_bar); out.add(e.sortino_bar);
    out.add(e.cagr); out.add(e.calmar); out.add(e.recovery_factor);
    out.add(e.time_in_market_pct); out.add(e.open_pl);
    out.add(report.equity_curve_len);
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i) {
        out.add(report.equity_curve[i].time_ms);
        out.add(report.equity_curve[i].equity);
        out.add(report.equity_curve[i].open_profit);
    }
    out.add(report.broker_state_hash_len);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        out.add(report.broker_state_hash[i]);
    std::printf("%s", row.name);
    for (const auto word : out.words) std::printf(" %016" PRIx64, word);
    std::printf("\n");
}

void percentage_witness(const std::array<pf_bar_t, kBars>& bars) {
    const double entry_cost = bars[1].open; // qty = point value = FX = 1
    const double pnl = bars[3].open - entry_cost;
    CHECK(entry_cost > 0.0 && std::isfinite(entry_cost));
    CHECK(std::isfinite(pnl) && std::isfinite(2.0 * pnl));
    CHECK(pnl == std::ldexp(1.0, 512));
    CHECK(!std::isfinite((pnl / entry_cost) * 100.0));
}

void run_row(const Row& row, bool emit_snapshot) {
    current_row = row.name;
    Host host{row};
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = static_cast<std::uint32_t>(sizeof(callbacks));
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &host;
    callbacks.on_bar = &on_bar;
    callbacks.on_applied = &on_applied;
    if (row.owns_excursions) callbacks.on_lot_excursion = &on_lot_excursion;
    host.handle = strategy_native_host_create_v1(&callbacks);
    CHECK(host.handle != nullptr);
    if (!host.handle) return;
    const auto base = spec();
    pf_native_run_spec_ext_v1 ext{};
    ext.struct_size = static_cast<std::uint32_t>(sizeof(ext));
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
    ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    ext.report_open_position_at_end = row.range_end ? 1u : 0u;
    const int configured = strategy_configure_native_ext_v1(host.handle, &base, &ext);
    CHECK(configured == PF_NATIVE_OK);
    if (configured != PF_NATIVE_OK) {
        strategy_native_host_free(host.handle);
        return;
    }
    strategy_set_broker_state_hash_recording(host.handle, 1);
    const auto bars = tape(row);
    if (row.percentage_overflow) percentage_witness(bars);
    pf_report_t report{};
    const int rc = strategy_native_run_v1(host.handle, bars.data(), kBars, &report);
    const char* code = strategy_get_last_error_code(host.handle);
    const std::string error_code = code ? code : "<null>";
    pf_native_state_v1 state{};
    state.struct_size = static_cast<std::uint32_t>(sizeof(state));
    CHECK(strategy_native_state_v1(host.handle, &state) == PF_NATIVE_OK);
    CHECK(host.command_error == PF_NATIVE_OK);
    CHECK(host.entries[0] != 0);
    if (!row.range_end) CHECK(host.entries[1] != 0 && host.entries[0] != host.entries[1]);
    CHECK(host.opened == row.lot_count());
    CHECK(host.live_lots == static_cast<std::uint64_t>(row.lot_count()));
    CHECK(host.live_units == static_cast<double>(row.lot_count()));
    if (row.owns_excursions) {
        CHECK(host.excursion_calls > 0);
        if (row.range_end) CHECK(host.excursion_calls == 1);
        if (row.expects_failure()) CHECK(host.bad_answers > 0);
    } else {
        CHECK(host.excursion_calls == 0);
    }

    if (row.expects_failure()) {
        CHECK(rc == PF_NATIVE_E_RUN_FAILED);
        CHECK(strategy_last_run_status(host.handle) == 1);
        CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_FAILED);
        CHECK(error_code == "engine_invariant");
        CHECK(report.total_trades == 0 && report.trades_len == 0 && report.trades == nullptr);
        CHECK(report.net_profit == 0.0);
        CHECK(host.closed == 0);
        CHECK(state.failure_code == (row.range_end ? PF_NATIVE_FAILURE_UNEXPECTED
                                                   : PF_NATIVE_FAILURE_SETTLEMENT_FAILURE));
        CHECK(state.failure_operation == (row.range_end ? PF_NATIVE_OPERATION_INPUT
                                                        : PF_NATIVE_OPERATION_SETTLEMENT));
        std::printf("%s: rc=%d code=%s trades=%d\n", row.name, rc, error_code.c_str(),
                    report.trades_len);
    } else {
        CHECK(rc == PF_NATIVE_OK);
        CHECK(strategy_last_run_status(host.handle) == 0);
        CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED);
        CHECK(error_code.empty());
        CHECK(report.total_trades == row.lot_count() && report.trades_len == row.lot_count());
        CHECK(report.input_bars_processed == kBars && report.script_bars_processed == kBars);
        CHECK(report.equity_curve_len == kBars && report.broker_state_hash_len == kBars);
        CHECK(host.closed == (row.range_end ? 0 : 1));
        if (report.trades) {
            for (int i = 0; i < report.trades_len; ++i) {
                const auto& trade = report.trades[i];
                CHECK(trade.entry_price == bars[1].open);
                CHECK(trade.exit_price == (row.range_end ? bars.back().close : bars[3].open));
                CHECK(trade.qty == 1.0 && trade.commission == 0.0 && trade.is_long == 1);
                CHECK(trade.pnl == trade.exit_price - trade.entry_price);
                CHECK(trade.pnl_pct == (trade.pnl / trade.entry_price) * 100.0);
                CHECK(trade.open_at_end == (row.range_end ? 1 : 0));
                CHECK(std::isfinite(trade.entry_price) && std::isfinite(trade.exit_price));
                CHECK(std::isfinite(trade.qty) && std::isfinite(trade.pnl));
                CHECK(std::isfinite(trade.pnl_pct) && std::isfinite(trade.commission));
                CHECK(std::isfinite(trade.max_runup) && std::isfinite(trade.max_drawdown));
                if (row.owns_excursions) {
                    CHECK(trade.max_runup == row.runup && trade.max_drawdown == row.drawdown);
                } else if (row.flat_tape) {
                    CHECK(trade.max_runup == 0.0 && trade.max_drawdown == 0.0);
                }
            }
        }
        if (emit_snapshot && report.trades && report.equity_curve && report.broker_state_hash)
            print_snapshot(row, report);
    }
    strategy_native_report_free_v1(&report);
    strategy_native_host_free(host.handle);
}

// Round b: separate fixtures leave every round-a row and assertion intact.
// One Transact{1}, then a whole-book Flatten: one lot, no survivor, no selected
// opening set and no lifecycle effects. OneLot::stage admits exactly this
// shape; the excursion owner does not disqualify the fused settlement.
std::vector<Row> one_lot_rows() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    return {
        {"one_lot_runup_nan", false, true, nan, 3.0},
        {"one_lot_drawdown_nan", false, true, 7.0, nan},
        {"one_lot_runup_posinf", false, true, inf, 3.0},
        {"one_lot_drawdown_posinf", false, true, 7.0, inf},
        {"one_lot_runup_neginf", false, true, -inf, 3.0},
        {"one_lot_drawdown_neginf", false, true, 7.0, -inf},
        {"one_lot_pnl_pct_overflow", false, false, 0.0, 0.0, true},
        {"finite_one_lot_zero", false, true, 0.0, 0.0},
        {"finite_one_lot_owned", false, true, 37.5, 12.25},
        {"finite_one_lot_signed", false, true, -7.5, -3.25},
        {"kernel_one_lot_normal", false, false, 0.0, 0.0},
    };
}

int on_one_lot_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& host = *static_cast<Host*>(user);
    ++host.calculations;
    if (host.calculations == 1) {
        host.submit(PF_NATIVE_INTENT_TRANSACT, "one-lot-entry", &host.entries[0]);
    } else if (host.calculations == 2) {
        CHECK(strategy_native_position_v1(host.handle, &host.live_units, nullptr,
                                         &host.live_lots) == PF_NATIVE_OK);
        CHECK(host.live_units == 1.0 && host.live_lots == 1);
    } else if (host.calculations == 3) {
        host.submit(PF_NATIVE_INTENT_FLATTEN, "one-lot-exit", nullptr);
    }
    return 0;
}

void run_one_lot_row(const Row& row, bool emit_snapshot) {
    current_row = row.name;
    Host host{row};
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = static_cast<std::uint32_t>(sizeof(callbacks));
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &host;
    callbacks.on_bar = &on_one_lot_bar;
    callbacks.on_applied = &on_applied;
    if (row.owns_excursions) callbacks.on_lot_excursion = &on_lot_excursion;
    host.handle = strategy_native_host_create_v1(&callbacks);
    CHECK(host.handle != nullptr);
    if (!host.handle) return;
    const auto base = spec();
    pf_native_run_spec_ext_v1 ext{};
    ext.struct_size = static_cast<std::uint32_t>(sizeof(ext));
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
    ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    ext.report_open_position_at_end = 0;
    const int configured = strategy_configure_native_ext_v1(host.handle, &base, &ext);
    CHECK(configured == PF_NATIVE_OK);
    if (configured != PF_NATIVE_OK) {
        strategy_native_host_free(host.handle);
        return;
    }
    strategy_set_broker_state_hash_recording(host.handle, 1);
    const auto bars = tape(row);
    if (row.percentage_overflow) percentage_witness(bars);
    pf_report_t report{};
    const int rc = strategy_native_run_v1(host.handle, bars.data(), kBars, &report);
    const char* code = strategy_get_last_error_code(host.handle);
    const std::string error_code = code ? code : "<null>";
    pf_native_state_v1 state{};
    state.struct_size = static_cast<std::uint32_t>(sizeof(state));
    CHECK(strategy_native_state_v1(host.handle, &state) == PF_NATIVE_OK);
    CHECK(host.command_error == PF_NATIVE_OK);
    CHECK(host.entries[0] != 0 && host.entries[1] == 0);
    CHECK(host.opened == 1 && host.live_lots == 1 && host.live_units == 1.0);
    if (row.owns_excursions) {
        CHECK(host.excursion_calls > 0);
        CHECK(row.expects_failure() ? host.bad_answers > 0 : host.bad_answers == 0);
    } else {
        CHECK(host.excursion_calls == 0);
    }
    if (row.expects_failure()) {
        // Pinned by the brief before execution: old two-field OneLot guards
        // admit the row; the settlement safety guard must fail before commit.
        CHECK(rc == PF_NATIVE_E_RUN_FAILED);
        CHECK(strategy_last_run_status(host.handle) == 1);
        CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_FAILED);
        CHECK(error_code == "engine_invariant");
        CHECK(state.failure_code == PF_NATIVE_FAILURE_SETTLEMENT_FAILURE);
        CHECK(state.failure_operation == PF_NATIVE_OPERATION_SETTLEMENT);
        CHECK(report.total_trades == 0 && report.trades_len == 0 && report.trades == nullptr);
        CHECK(report.net_profit == 0.0 && host.closed == 0);
        std::printf("%s: rc=%d code=%s trades=%d\n", row.name, rc, error_code.c_str(),
                    report.trades_len);
    } else {
        CHECK(rc == PF_NATIVE_OK && strategy_last_run_status(host.handle) == 0);
        CHECK(state.lifecycle == PF_NATIVE_LIFECYCLE_COMPLETED && error_code.empty());
        CHECK(host.closed == 1 && host.calculations == kBars);
        CHECK(report.total_trades == 1 && report.trades_len == 1 && report.trades != nullptr);
        CHECK(report.input_bars_processed == kBars && report.script_bars_processed == kBars);
        CHECK(report.equity_curve_len == kBars && report.broker_state_hash_len == kBars);
        CHECK(report.equity_curve != nullptr && report.broker_state_hash != nullptr);
        if (report.trades && report.trades_len == 1) {
            const auto& trade = report.trades[0];
            CHECK(trade.entry_price == bars[1].open && trade.exit_price == bars[3].open);
            CHECK(trade.qty == 1.0 && trade.is_long == 1 && trade.open_at_end == 0);
            CHECK(trade.pnl == 2.0 && trade.pnl_pct == (2.0 / 101.0) * 100.0);
            CHECK(trade.commission == 0.0);
            CHECK(std::isfinite(trade.max_runup) && std::isfinite(trade.max_drawdown));
            if (row.owns_excursions) {
                CHECK(trade.max_runup == row.runup && trade.max_drawdown == row.drawdown);
            }
        }
        if (emit_snapshot && report.trades && report.equity_curve && report.broker_state_hash)
            print_snapshot(row, report);
    }
    strategy_native_report_free_v1(&report);
    strategy_native_host_free(host.handle);
}

#ifdef PINEFORGE_SETTLEMENT_SOURCE_HEADER

void snapshot_trade(Snapshot& out, const pineforge::TradeC& t) {
    out.add(t.entry_time); out.add(t.exit_time);
    out.add(t.entry_price); out.add(t.exit_price);
    out.add(t.pnl); out.add(t.pnl_pct); out.add(t.is_long);
    out.add(t.max_runup); out.add(t.max_drawdown);
    out.add(t.qty); out.add(t.commission);
    out.add(t.entry_bar_index); out.add(t.exit_bar_index); out.add(t.open_at_end);
}

void print_source_snapshot(const char* name, const pineforge::ReportC& report) {
    // The public C report layout is pinned by native_module.hpp / c_abi.cpp.
    // Copy values across that layout and give the C snapshot its own typed
    // trade storage, so it never dereferences a TradeC as a pf_trade_t.
    pf_report_t portable{};
    static_assert(sizeof(portable) == sizeof(report), "public report layout");
    std::memcpy(&portable, &report, sizeof(portable));
    std::vector<pf_trade_t> trades(static_cast<std::size_t>(report.trades_len));
    static_assert(sizeof(pf_trade_t) == sizeof(pineforge::TradeC), "public trade layout");
    for (int index = 0; index < report.trades_len; ++index)
        std::memcpy(&trades[static_cast<std::size_t>(index)], &report.trades[index],
                    sizeof(pf_trade_t));
    portable.trades = trades.data();
    print_snapshot({name, false, false, 0.0, 0.0}, portable);
}

pf_strategy_t source_handle(pineforge::source::PineStrategyHost& host) {
    return static_cast<pf_strategy_t>(static_cast<pineforge::BacktestEngine*>(&host));
}

void select_source_window(pineforge::source::PineStrategyHost& host, std::int64_t end) {
    pf_selected_window_config_v1 window{};
    window.struct_size = static_cast<std::uint32_t>(sizeof(window));
    window.version = 1;
    window.start_ms = kStart;
    window.end_ms = end;
    CHECK(strategy_set_selected_window_v1(source_handle(host), &window) == 0);
}

void check_finite_source_trade(const pineforge::Trade& trade) {
    CHECK(std::isfinite(trade.entry_price) && std::isfinite(trade.exit_price));
    CHECK(std::isfinite(trade.qty) && std::isfinite(trade.pnl));
    CHECK(std::isfinite(trade.pnl_pct) && std::isfinite(trade.commission));
    CHECK(std::isfinite(trade.max_runup) && std::isfinite(trade.max_drawdown));
}

void check_source_failure(pineforge::source::PineStrategyHost& host,
                          const char* message, const pineforge::ReportC& report) {
    CHECK(host.last_run_status() == 1);
    CHECK(host.native_state().kind == pineforge::NativeLifecycleKind::Failed);
    const char* code = strategy_get_last_error_code(source_handle(host));
    CHECK(code != nullptr && std::strcmp(code, "engine_invariant") == 0);
    CHECK(host.last_error() == message);
    CHECK(report.total_trades == 0 && report.trades_len == 0 && report.trades == nullptr);
    CHECK(report.equity_curve == nullptr && report.equity_curve_len == 0);
    std::printf("%s: code=%s trades=%d\n", current_row, code ? code : "<null>",
                report.trades_len);
}

enum class QuoteCase { Finite, PercentageOverflow, PriceOverflow };

class TerminalQuoteHost final : public pineforge::source::PineStrategyHost {
public:
    TerminalQuoteHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.margin_long = config.margin_short = 0.0;
        config.commission_type = static_cast<int>(pineforge::CommissionType::CASH_PER_CONTRACT);
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
    }
    void on_source_bar(const pineforge::Bar&) override {
        if (bar_index_ == 0) strategy_entry("quote-entry", true);
        if (bar_index_ == 1) CHECK(signed_position_size() == 1.0);
    }
};

void run_terminal_quote_row(const char* name, QuoteCase which, bool emit_snapshot) {
    current_row = name;
    const bool fault = which != QuoteCase::Finite;
    const double entry = fault ? std::ldexp(1.0, -512) : 100.0;
    const double quote = fault ? std::ldexp(1.0, 512) : 112.0;
    pineforge::SymInfo instrument;
    instrument.mintick = which == QuoteCase::PriceOverflow ? std::ldexp(1.0, -600) : 0.0;
    instrument.pointvalue = 1.0;
    std::vector<pineforge::Bar> bars;
    for (int index = 0; index <= 60; ++index)
        bars.push_back({entry, entry, entry, entry, 1.0, kStart + index * 60000LL});
    TerminalQuoteHost host;
    const auto quote_time = bars.back().timestamp;
    CHECK(host.set_report_terminal_quote(quote_time, quote));
    select_source_window(host, kStart + 75 * 60000LL);
    if (which == QuoteCase::PercentageOverflow) {
        CHECK(std::isfinite(quote - entry));
        CHECK(!std::isfinite(((quote - entry) / entry) * 100.0));
    } else if (which == QuoteCase::PriceOverflow) {
        CHECK(std::isfinite(entry / instrument.mintick));
        CHECK(!std::isfinite(quote / instrument.mintick));
    }
    // Same selected run, differing only in the absent terminal quote. Read
    // its presented row through the public report surface; no override calls
    // the host's private presentation hook or changes a product report.
    TerminalQuoteHost unquoted_host;
    select_source_window(unquoted_host, kStart + 75 * 60000LL);
    unquoted_host.run(bars.data(), static_cast<int>(bars.size()), "1", "15", {}, instrument,
                      nullptr, true);
    pineforge::ReportC unquoted{};
    unquoted_host.fill_report(&unquoted);
    CHECK(unquoted_host.last_run_status() == 0 && unquoted_host.last_error().empty());
    CHECK(unquoted_host.native_state().kind == pineforge::NativeLifecycleKind::Completed);
    CHECK(!unquoted_host.report_terminal_quote_applied(unquoted));
    CHECK(unquoted.total_trades == 1 && unquoted.trades_len == 1 && unquoted.trades != nullptr);
    Snapshot before;
    if (unquoted.trades && unquoted.trades_len == 1) {
        auto row = unquoted.trades[0];
        CHECK(row.open_at_end == 1 && row.is_long == 1);
        CHECK(row.entry_price == entry && row.exit_price == entry);
        CHECK(row.qty == 1.0 && row.pnl == 0.0 && row.pnl_pct == 0.0);
        CHECK(row.commission == 0.0);
        CHECK(std::isfinite(row.max_runup) && std::isfinite(row.max_drawdown));
        // Exit time may change with the quote; normalize both copies to a
        // literal before comparing every field the quote must leave intact.
        row.exit_time = 0;
        snapshot_trade(before, row);
    }
    CHECK(before.words.size() == 14);
    CHECK(unquoted.net_profit == 0.0 && unquoted.metrics.all.net_profit == 0.0);
    CHECK(unquoted.equity_curve_len > 0 && unquoted.equity_curve != nullptr);
    if (unquoted.equity_curve && unquoted.equity_curve_len > 0)
        CHECK(unquoted.equity_curve[unquoted.equity_curve_len - 1].equity == 10000.0);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "15", {}, instrument,
             nullptr, true);
    pineforge::ReportC report{};
    host.fill_report(&report);
    CHECK(host.trade_count() == 0 && host.report_trade_count() == 1);
    if (host.report_trade_count() == 1) {
        const auto& original = host.get_report_trade(0);
        check_finite_source_trade(original);
        CHECK(original.open_at_end && original.is_long && original.qty == 1.0);
        CHECK(original.entry_price == entry && original.exit_price == entry);
        CHECK(original.pnl == 0.0 && original.pnl_pct == 0.0);
        CHECK(original.commission == 0.0);
    }
    if (fault) {
        check_source_failure(host, "non-finite terminal-quote report row", report);
        CHECK(host.native_state().failure.code == pineforge::NativeFailureCode::Unexpected);
        CHECK(host.native_state().failure.operation == pineforge::NativeFailureOperation::Input);
    } else {
        CHECK(host.last_run_status() == 0 && host.last_error().empty());
        CHECK(host.native_state().kind == pineforge::NativeLifecycleKind::Completed);
        CHECK(report.trades_len == 1 && report.trades != nullptr);
        CHECK(report.equity_curve_len > 0 && report.equity_curve != nullptr);
        if (report.trades && report.trades_len == 1 && report.equity_curve) {
            CHECK(host.report_terminal_quote_applied(report));
            auto projected = report.trades[0];
            CHECK(projected.entry_price == 100.0 && projected.qty == 1.0);
            CHECK(projected.open_at_end == 1 && projected.is_long == 1);
            CHECK(projected.exit_time == quote_time && projected.exit_price == quote);
            CHECK(projected.pnl == 12.0 && projected.pnl_pct == 12.0);
            CHECK(projected.commission == 0.0);
            projected.exit_time = 0;
            projected.exit_price = entry;
            projected.pnl = projected.pnl_pct = 0.0;
            Snapshot normalized;
            snapshot_trade(normalized, projected);
            CHECK(normalized.words == before.words);
            CHECK(report.net_profit == 12.0 && report.metrics.all.net_profit == 12.0);
            const auto& last = report.equity_curve[report.equity_curve_len - 1];
            CHECK(last.time_ms == quote_time && last.equity == 10012.0);
            pineforge::ReportC repeated{};
            host.fill_report(&repeated);
            CHECK(repeated.trades_len == 1 && repeated.trades != nullptr);
            if (repeated.trades && repeated.trades_len == 1) {
                Snapshot left, right;
                snapshot_trade(left, report.trades[0]);
                snapshot_trade(right, repeated.trades[0]);
                CHECK(left.words == right.words && repeated.net_profit == report.net_profit);
            }
            pineforge::BacktestEngine::free_report(&repeated);
            if (emit_snapshot) print_source_snapshot(name, report);
        }
    }
    pineforge::BacktestEngine::free_report(&report);
    pineforge::BacktestEngine::free_report(&unquoted);
}

class AmendedTradeHost final : public pineforge::source::PineStrategyHost {
public:
    mutable ReportCaptureFixture presented;
    explicit AmendedTradeHost(bool overflow) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = overflow ? 1e303 : 10000.0;
        config.pyramiding = 2;
        config.margin_long = config.margin_short = 0.0;
        config.close_entries_rule_any = false;
        config.commission_type = static_cast<int>(pineforge::CommissionType::CASH_PER_CONTRACT);
        config.commission_value = overflow ? 1e290 : 0.25;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
        scale = overflow ? 1e10 : 1.0;
    }
    void on_source_bar(const pineforge::Bar&) override {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        // B hashes into bucket 2, A into bucket 1. The global exit's child
        // order is A,B, while the book is B,A, forcing FIFO re-slicing.
        if (bar_index_ == 0) strategy_entry("B", true, nan, nan, 3.0 * scale);
        if (bar_index_ == 1) strategy_entry("A", true, nan, nan, 7.0 * scale);
        if (bar_index_ == 2) {
            CHECK(signed_position_size() == 10.0 * scale);
            strategy_exit("global", "", 110.0, nan);
            exit_submitted = true;
        }
    }
    double scale = 1.0;
    bool exit_submitted = false;
    void present_report(pineforge::ReportC* out) const override {
        pineforge::source::PineStrategyHost::present_report(out);
        presented.save(*out);
    }
};

void run_amended_trade_row(const char* name, bool overflow, bool emit_snapshot) {
    current_row = name;
    AmendedTradeHost host(overflow);
    const double scale = overflow ? 1e10 : 1.0;
    const double fee = overflow ? 1e290 : 0.25;
    // The two whole-lot commissions and their sum are finite; the adapter's
    // transaction fee times the first slice overflows BEFORE its division.
    CHECK(std::isfinite(2.0 * fee * (10.0 * scale)));
    const double transaction_fee = fee * (7.0 * scale);
    CHECK(std::isfinite(transaction_fee));
    CHECK(overflow ? !std::isfinite(transaction_fee * (3.0 * scale))
                   : std::isfinite(transaction_fee * (3.0 * scale)));
    std::array<pineforge::Bar, 5> bars{};
    for (int index = 0; index < 5; ++index) {
        const double price = index < 3 ? 100.0 : 110.0;
        bars[static_cast<std::size_t>(index)] =
            {price, price, price, price, 1.0, kStart + index * 60000LL};
    }
    select_source_window(host, kStart + 5 * 60000LL);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    pineforge::ReportC report{};
    if (overflow) {
        host.fill_report(&report);
    } else {
        // Appendix A:83/93: the finite settlement makes 95 / 10000 in four
        // minutes, whose CAGR overflows. Observe its pre-metric presentation
        // to retain every settlement assertion; the result itself must fail.
        CHECK(report_failure_checks::rejects_nonfinite("metrics.equity.cagr", [&] {
            host.fill_report(&report);
        }));
        CHECK(report.trades == nullptr && report.equity_curve == nullptr);
    }
    CHECK(host.exit_submitted);
    if (overflow) {
        check_source_failure(host, "non-finite amended trade row", report);
        CHECK(host.native_state().failure.code == pineforge::NativeFailureCode::CallbackException);
        CHECK(host.native_state().failure.operation == pineforge::NativeFailureOperation::Callback);
        // Failure must preserve the TWO finite, already committed rows. A
        // guard after resize/push would expose three rows or a partial rewrite.
        CHECK(host.trade_count() == 2);
        for (int index = 0; index < host.trade_count(); ++index)
            check_finite_source_trade(host.get_trade(index));
        if (host.trade_count() == 2) {
            CHECK(host.get_trade(0).entry_id == "B" && host.get_trade(0).qty == 3.0 * scale);
            CHECK(host.get_trade(1).entry_id == "A" && host.get_trade(1).qty == 7.0 * scale);
        }
    } else {
        const auto& presentation = host.presented.raw;
        CHECK(host.last_run_status() == 0 && host.last_error().empty());
        CHECK(host.native_state().kind == pineforge::NativeLifecycleKind::Completed);
        CHECK(host.trade_count() == 3 && presentation.trades_len == 3 && presentation.trades != nullptr);
        constexpr double quantities[] = {3.0, 4.0, 3.0};
        constexpr double commissions[] = {1.5, 2.0, 1.5};
        constexpr double profits[] = {28.5, 38.0, 28.5};
        for (int index = 0; index < host.trade_count(); ++index)
            check_finite_source_trade(host.get_trade(index));
        if (presentation.trades && presentation.trades_len == 3) {
            for (int index = 0; index < 3; ++index) {
                const auto& row = presentation.trades[index];
                CHECK(row.entry_price == 100.0 && row.exit_price == 110.0);
                CHECK(row.qty == quantities[index] && row.commission == commissions[index]);
                CHECK(row.pnl == profits[index] && row.pnl_pct == 9.5);
                CHECK(row.open_at_end == 0 && row.is_long == 1);
            }
            const auto stats = pineforge::metrics::compute_trade_stats(
                presentation.trades, presentation.trades_len,
                pineforge::metrics::TradeFilter::ALL, 10000.0);
            CHECK(presentation.net_profit == 95.0 && stats.net_profit == 95.0);
            if (emit_snapshot)
                std::printf("%s: report_result_invalid field=metrics.equity.cagr reason=nonfinite\n",
                            name);
        }
    }
    pineforge::BacktestEngine::free_report(&report);
}

struct SourceRow {
    const char* name;
    bool fault;
    bool amendment;
    QuoteCase quote;
};

constexpr SourceRow source_rows[] = {
    {"terminal_quote_pnl_pct_overflow", true, false, QuoteCase::PercentageOverflow},
    {"terminal_quote_price_overflow", true, false, QuoteCase::PriceOverflow},
    {"finite_terminal_quote", false, false, QuoteCase::Finite},
    {"amended_trade_commission_overflow", true, true, QuoteCase::Finite},
    {"finite_amended_trade", false, true, QuoteCase::Finite},
};

#endif // PINEFORGE_SETTLEMENT_SOURCE_HEADER

} // namespace

int main(int argc, char** argv) {
    const bool controls = argc == 2 && std::strcmp(argv[1], "--controls") == 0;
    if (argc > 2) {
        std::fprintf(stderr, "usage: test_native_settlement_row_finite [row-name|--controls]\n");
        return 2;
    }
    int ran = 0;
    for (const auto& row : rows()) {
        if (controls ? row.expects_failure() : argc == 2 && std::strcmp(argv[1], row.name) != 0)
            continue;
        run_row(row, controls);
        ++ran;
    }
    for (const auto& row : one_lot_rows()) {
        if (controls ? row.expects_failure() : argc == 2 && std::strcmp(argv[1], row.name) != 0)
            continue;
        run_one_lot_row(row, controls);
        ++ran;
    }
#ifdef PINEFORGE_SETTLEMENT_SOURCE_HEADER
    for (const auto& row : source_rows) {
        if (controls ? row.fault : argc == 2 && std::strcmp(argv[1], row.name) != 0)
            continue;
        if (row.amendment) run_amended_trade_row(row.name, row.fault, controls);
        else run_terminal_quote_row(row.name, row.quote, controls);
        ++ran;
    }
#endif
    if (ran == 0) {
        std::fprintf(stderr, "unknown settlement row\n");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "test_native_settlement_row_finite: %d failure(s), %d rows\n",
                     failures, ran);
        return 1;
    }
    std::printf("test_native_settlement_row_finite: ok (%d rows)\n", ran);
    return 0;
}
