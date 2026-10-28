#include "report_replay.hpp"
#include "engine_internal.hpp"
#include "native_execution_consumer.hpp"
#include "report_value_invalid.hpp"
#include "report_sink_writer.hpp"

#include <pineforge/version.h>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <utility>

namespace pineforge {
namespace report {
namespace {
constexpr std::uint64_t safe_integer = UINT64_C(9007199254740991);

[[noreturn]] void invalid(const char* text) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, text);
}

std::string integer(std::uint64_t value) {
    if (value > safe_integer) invalid("replay index exceeds the exact JSON integer bound");
    return std::to_string(value);
}

std::string real(double value, const char* field) {
    return report_sink::f64(finite_value(value, field));
}

const char* basis_name(EquityBasis basis) {
    switch (basis) {
    case EquityBasis::kernel_open: return "kernel_open";
    case EquityBasis::range_end: return "range_end";
    case EquityBasis::terminal_quote: return "terminal_quote";
    case EquityBasis::anchor: return "anchor";
    }
    invalid("unknown replay equity basis");
}

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// Fixed descriptors belong to the companion only. Primary E/T are untouched.
const char* descriptors =
    "[{\"type\":\"S\",\"fields\":[\"initial_capital\",\"pointvalue\",\"net_profit_sum\"],"
    "\"types\":[\"f64\",\"f64\",\"f64\"],\"units\":[\"account_ccy\",\"quote_ccy\",\"account_ccy\"]},"
    "{\"type\":\"Q\",\"fields\":[\"point_index\",\"time_ms\",\"settlement_count\",\"net_profit_sum\","
    "\"equity_basis\",\"equity_term\"],\"types\":[\"i64\",\"i64\",\"i64?\",\"f64?\",\"str\",\"f64\"],"
    "\"units\":[\"\",\"unix_ms\",\"\",\"account_ccy\",\"\",\"account_ccy\"]},"
    "{\"type\":\"R\",\"fields\":[\"n\",\"settlement_index\",\"parent_id\",\"slice_index\","
    "\"slice_count\",\"recut_fields\",\"child_index\",\"transaction_qty\",\"whole_qty\","
    "\"whole_pnl\",\"whole_commission\",\"whole_max_runup\",\"whole_max_drawdown\","
    "\"whole_exit_fee\",\"transaction_fee\"],"
    "\"types\":[\"i64\",\"i64?\",\"str?\",\"i64?\",\"i64?\",\"str[]?\",\"i64?\","
    "\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\"],"
    "\"units\":[\"\",\"\",\"\",\"\",\"\",\"\",\"\",\"qty\",\"qty\",\"account_ccy\","
    "\"account_ccy\",\"account_ccy\",\"account_ccy\",\"account_ccy\",\"account_ccy\"]}]";
}

ReportReplay::ReportReplay(std::string path, std::string run_id)
    : path_(std::move(path)), run_id_(std::move(run_id)) {}

std::unique_ptr<ReportReplay> ReportReplay::configuration_copy() const {
    return std::make_unique<ReportReplay>(path_, run_id_);
}

void ReportReplay::begin(const std::string& scratch_dir, double capital, double pointvalue,
                         bool selected, std::int64_t start, std::int64_t end) {
    writer_.reset();
    trades_.clear();
    points_.clear();
    directory_ = scratch_dir;
    trades_.set_directory(directory_);
    points_.set_directory(directory_);
    trade_base_ = 0;
    settlements_ = q_count_ = r_count_ = 0;
    capital_ = capital;
    pointvalue_ = pointvalue;
    selected_ = selected;
    last_selected_ = complete_ = false;
    start_ = start;
    end_ = end;
    if (selected_) points_.push_back({start_, 0, 0.0, 0.0, EquityBasis::anchor});
    const pf_report_sink_v1 sink{sizeof(pf_report_sink_v1), 1,
        path_.c_str(), run_id_.c_str(), directory_.c_str()};
    writer_ = std::make_unique<report_sink::LineWriter>(sink);
    writer_->begin_companion(
        "{\"schema\":\"pf-report-replay\",\"version\":1,\"run_id\":"
        + report_sink::escaped_string(run_id_) + ",\"engine\":"
        + report_sink::escaped_string(PINEFORGE_VERSION_STRING) + ",\"records\":"
        + descriptors + "}");
}

void ReportReplay::close(std::size_t global) {
    if (complete_ || global != trade_base_ + trades_.size() || settlements_ == safe_integer)
        invalid("replay close cursor is outside the resident window");
    ReplayTrade row;
    row.settlement = settlements_;
    trades_.push_back(row);
    ++settlements_;
}

ReplayTrade ReportReplay::trade(std::size_t global) const {
    if (global < trade_base_) invalid("replay row has already been retired");
    return trades_.at(global - trade_base_);
}

void ReportReplay::replace(std::size_t global, const ReplayTrade& value) {
    if (global < trade_base_) invalid("replay replacement precedes the resident window");
    trades_.set(global - trade_base_, value);
}

void ReportReplay::resize_trades(std::size_t global_end) {
    if (global_end < trade_base_) invalid("replay resize precedes the resident window");
    trades_.resize(global_end - trade_base_);
}

void ReportReplay::retire(std::size_t global_end) {
    if (global_end < trade_base_ || global_end - trade_base_ > trades_.size()
        || global_end > r_count_)
        invalid("replay retirement precedes successful R emission");
    trades_.erase(trades_.begin(), trades_.begin() + (global_end - trade_base_));
    if (trades_.empty()) trades_.clear();
    trade_base_ = global_end;
}

void ReportReplay::point(const pf_equity_point_t& value, double net) {
    last_selected_ = !selected_ || (value.time_ms >= start_ && value.time_ms < end_);
    if (!last_selected_) return;
    points_.push_back({value.time_ms, settlements_, net, value.open_profit,
                      EquityBasis::kernel_open});
}

void ReportReplay::amend_point(std::int64_t time, EquityBasis basis, double term, double net) {
    if (complete_ || !last_selected_) return;
    if (points_.empty()) invalid("replay presentation lost its held point");
    auto row = points_.at(points_.size() - 1);
    row.time = time;
    row.basis = basis;
    row.term = term;
    row.net = net;
    row.settlements = settlements_;
    points_.set(points_.size() - 1, row);
}

void ReportReplay::write_q(const pf_equity_point_t& point) {
    if (!writer_ || complete_ || points_.empty())
        invalid("replay Q has no recorded operands");
    const auto row = points_.at(0);
    if (row.time < -static_cast<std::int64_t>(safe_integer)
        || row.time > static_cast<std::int64_t>(safe_integer))
        invalid("replay time exceeds the exact JSON integer bound");
    const double expected = row.basis == EquityBasis::anchor ? capital_
        : row.basis == EquityBasis::terminal_quote ? capital_ + row.term
        : (capital_ + row.net) + row.term;
    if (row.time != point.time_ms || !same_bits(expected, point.equity))
        throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
            {{"shape", "replay_presenter"}},
            "exact replay unavailable: presenter did not retain its equity operands");
    std::string line = "[\"Q\"," + integer(q_count_) + "," + std::to_string(row.time) + ",";
    if (row.basis == EquityBasis::anchor) line += "null,null,";
    else line += integer(row.settlements) + "," + real(row.net, "equity_curve.net_profit_sum") + ",";
    line += report_sink::escaped_string(basis_name(row.basis)) + ","
        + real(row.term, "equity_curve.equity_term") + "]";
    writer_->companion_record(line);
    (void)integer(q_count_ + 1);
    ++q_count_;
    points_.erase(points_.begin(), points_.begin() + 1);
    if (points_.empty()) points_.clear();
}

void ReportReplay::write_r(std::size_t global, bool terminal) {
    if (!writer_ || complete_ || global != r_count_)
        invalid("replay R does not match its global T ordinal");
    const ReplayTrade row = terminal ? ReplayTrade{} : trade(global);
    std::string line = "[\"R\"," + integer(r_count_ + 1) + ","
        + (terminal ? "null" : integer(row.settlement));
    if (!row.marked) {
        for (int i = 0; i < 13; ++i) line += ",null";
    } else {
        line += "," + report_sink::escaped_string(std::to_string(row.parent))
            + "," + integer(row.slice) + "," + integer(row.slices) + ","
            + (row.fifo ? "[\"qty\",\"commission\",\"pnl\",\"pnl_pct\",\"max_runup\",\"max_drawdown\"]" : "[]")
            + "," + integer(row.child);
        const double values[] = {row.transaction_qty, row.whole_qty, row.whole_pnl,
            row.whole_commission, row.whole_runup, row.whole_drawdown};
        const char* fields[] = {"trades.recut.transaction_qty", "trades.recut.whole_qty",
            "trades.recut.whole_pnl", "trades.recut.whole_commission",
            "trades.recut.whole_max_runup", "trades.recut.whole_max_drawdown"};
        for (std::size_t i = 0; i < 6; ++i) line += "," + real(values[i], fields[i]);
        line += row.fees ? "," + real(row.whole_exit_fee, "trades.recut.whole_exit_fee")
            + "," + real(row.transaction_fee, "trades.recut.transaction_fee") : ",null,null";
    }
    line += "]";
    writer_->companion_record(line);
    ++r_count_;
}

void ReportReplay::finish(double net, std::string_view base_sha256) {
    if (complete_) return;
    if (!writer_ || !points_.empty()) invalid("replay companion has unconsumed equity points");
    writer_->companion_record("[\"S\"," + real(capital_, "replay.initial_capital") + ","
        + real(pointvalue_, "replay.pointvalue") + "," + real(net, "replay.net_profit_sum") + "]");
    writer_->finish_companion("{\"S\":1,\"Q\":" + integer(q_count_)
        + ",\"R\":" + integer(r_count_) + "}", base_sha256);
    complete_ = true;
    trades_.clear();
    points_.clear();
}
} // namespace report

int BacktestEngine::set_report_replay_v1(const char* path, const char* run_id) {
    const int status = report_sink_setter_status();
    if (status != 0) return status;
    if (trades_.base() != 0) return -3;
    if (!path) {
        report_sink_state_.replay.reset();
        return 0;
    }
    // Bound new configuration allocations before copying either C string.
    constexpr std::size_t max_text = 64 * 1024;
    if (!*path || ::strnlen(path, max_text + 1) > max_text
        || (run_id && ::strnlen(run_id, max_text + 1) > max_text)
        || (report_sink_state_.enabled && path == report_sink_state_.path)) return -1;
    report_sink_state_.replay = std::make_unique<report::ReportReplay>(
        path, run_id ? run_id : "");
    return 0;
}

double BacktestEngine::report_pointvalue() const noexcept {
    const auto* replay = report_sink_state_.replay.get();
    return replay && replay->begun() ? replay->pointvalue() : syminfo_.pointvalue;
}

void BacktestEngine::begin_report_replay() {
    auto& sink = report_sink_state_;
    struct stat primary{}, companion{};
    if (sink.enabled && sink.fd < 0
        && (sink.path == sink.replay->path()
            || (::stat(sink.path.c_str(), &primary) == 0
                && ::stat(sink.replay->path().c_str(), &companion) == 0
                && primary.st_dev == companion.st_dev && primary.st_ino == companion.st_ino)))
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                      "primary and replay sinks must be separate files");
    if (pending_equity_report_mode_ == ReportMode::SUMMARY_ONLY)
        throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
            {{"shape", "replay_summary"}}, "exact replay requires recorded equity points");
    const auto* consumer = execution_consumer_slot_.ptr
        ? &as_native_consumer(*execution_consumer_slot_.ptr) : nullptr;
    const auto* spec = consumer ? consumer->view().spec : nullptr;
    if (spec && spec->report_policy == NativeReportPolicy::HostRecorded)
        throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
            {{"shape", "replay_host_curve"}}, "exact replay requires recorded equity operands");
    pf_selected_window_config_v1 window{};
    const bool selected = NativeExecutionConsumer::selected_window_admitted(*this, &window);
    sink.replay->begin(sink.enabled ? sink.scratch_dir : trades_.scratch_directory(),
        initial_capital_, syminfo_.pointvalue, selected, window.start_ms, window.end_ms);
}

void BacktestEngine::record_report_replay_close(std::size_t global) {
    report_sink_state_.replay->close(global);
}
void BacktestEngine::record_report_replay_point(const pf_equity_point_t& point) {
    report_sink_state_.replay->point(point, net_profit_sum_);
}

bool BacktestEngine::prepare_special_equity_point() {
    if (active_equity_report_mode_ == ReportMode::SUMMARY_ONLY) return equity_curve_.empty();
    const auto* held = last_equity_point();
    const bool first = prepare_equity_point();
    // Ordinary prepare just released this E. Selected capture releases its
    // complete presented window later; array capture releases nothing here.
    const auto& sink = report_sink_state_;
    if (held && sink.enabled && sink.writer && !sink.selected_equity_stream)
        write_report_replay_equity(*held);
    return first;
}

void BacktestEngine::store_special_equity_point(const pf_equity_point_t& p) {
    if (active_equity_report_mode_ != ReportMode::SUMMARY_ONLY) {
        record_report_replay_point(p);
        store_equity_point(p);
        return;
    }
    // The existing summary fold, unchanged in operation and value order.
    if (!equity_curve_.empty()) {
        const auto& held = equity_curve_.back();
        summary_equity_fold_.replace_last(
            held, metrics::equity_month_key(held.time_ms, chart_timezone_));
        auto& prefix = summary_broker_equity_prefix_;
        const double eq = held.equity;
        if (eq > prefix[0]) prefix[0] = prefix[1] = eq;
        if (eq < prefix[1]) prefix[1] = eq;
        const double dd = prefix[0] - eq;
        if (dd > prefix[2]) prefix[2] = dd;
        const double ru = eq - prefix[1];
        if (ru > prefix[3]) prefix[3] = ru;
    }
    summary_equity_fold_.fold(
        p, metrics::equity_month_key(p.time_ms, chart_timezone_),
        true, position_side_ != PositionSide::FLAT);
    if (equity_curve_.empty()) equity_curve_.push_back(p);
    else equity_curve_.back() = p;
    summary_equity_snapshot_valid_ = summary_equity_ratios_ready_ = false;
    summary_equity_ratios_.reset();
}

void BacktestEngine::amend_report_replay_point(
        std::int64_t time, report::EquityBasis basis, double term) const {
    if (report_sink_state_.replay)
        report_sink_state_.replay->amend_point(time, basis, term, net_profit_sum_);
}
void BacktestEngine::write_report_replay_equity(const pf_equity_point_t& point) const {
    report_sink_state_.replay->write_q(point);
}
void BacktestEngine::write_report_replay_trade(std::size_t global, bool terminal) const {
    report_sink_state_.replay->write_r(global, terminal);
}
void BacktestEngine::retire_report_replay_trades(std::size_t global_end) {
    report_sink_state_.replay->retire(global_end);
}

void BacktestEngine::finish_report_replay(const ReportC* out) const {
    auto& sink = report_sink_state_;
    auto& replay = *sink.replay;
    const auto* consumer = execution_consumer_slot_.ptr
        ? &as_native_consumer(*execution_consumer_slot_.ptr) : nullptr;
    if (replay.complete() || last_run_status_ != 0 || !last_error_.empty()
        || !consumer || consumer->state_kind() != NativeLifecycleKind::Completed) return;
    if (sink.enabled) {
        if (!sink.receipt.complete) return;
        if (replay.r_count() != static_cast<std::uint64_t>(sink.receipt.count_t)
            || replay.q_count() != static_cast<std::uint64_t>(sink.receipt.count_e))
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                          "replay coverage differs from the primary receipt");
    } else {
        if (out->trades_len != out->total_trades)
            throw coded<std::logic_error>(RunFailureCode::report_shape_unsupported,
                {{"shape", "replay_presenter"}}, "exact replay requires every presented row");
        for (std::int64_t i = 0; i < out->equity_curve_len; ++i)
            replay.write_q(out->equity_curve[i]);
        for (int i = 0; i < out->trades_len; ++i)
            replay.write_r(static_cast<std::size_t>(i), out->trades[i].open_at_end != 0);
    }
    replay.finish(net_profit_sum_,
                  sink.enabled ? std::string_view(sink.receipt.sha256_hex) : std::string_view{});
}
} // namespace pineforge
