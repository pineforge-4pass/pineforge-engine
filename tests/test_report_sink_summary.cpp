// CX-SINK-B5a: real runs, with an independent array-report reference.
// Fail-before: the previous report_sink.h has no pf_report_sink_summary_v1.
// The supervisor records the frozen-header compiler diagnostic on the lab.
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/execution_observer.h>
#include <pineforge/selected_window.h>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using namespace pineforge;
constexpr std::int64_t start_ms = INT64_C(1736121600000);
constexpr std::int64_t minute = 60000;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool same(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}
pf_strategy_t handle(BacktestEngine& host) { return &host; }

struct Report {
    ReportC value{};
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
    ~Report() { BacktestEngine::free_report(&value); }
};
struct Directory {
    std::string path;
    std::vector<std::string> files;
    Directory() {
        char pattern[] = "/tmp/pineforge-l22q-XXXXXX";
        const char* made = ::mkdtemp(pattern);
        require(made != nullptr, "mkdtemp failed");
        path = made;
    }
    ~Directory() {
        for (const auto& file : files) std::remove(file.c_str());
        ::rmdir(path.c_str());
    }
    std::string file() {
        files.push_back(path + "/" + std::to_string(files.size()) + ".ndjson");
        return files.back();
    }
};

// Drain a real FIFO while the engine writes. Tests do not parse these bytes:
// the reference is the independent sink-OFF engine's own report array.
class Pipe {
public:
    Pipe() {
        int descriptors[2];
        require(::pipe(descriptors) == 0, "pipe failed");
        reader_ = descriptors[0];
        writer_ = descriptors[1];
        try {
            thread_ = std::thread([this] {
                std::array<char, 4096> bytes{};
                for (;;) {
                    const auto count = ::read(reader_, bytes.data(), bytes.size());
                    if (count < 0 && errno == EINTR) continue;
                    if (count < 0) { failed_ = true; break; }
                    if (count == 0) break;
                }
                ::close(reader_);
            });
        } catch (...) {
            ::close(reader_);
            ::close(writer_);
            throw;
        }
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() { close(); }
    int fd() const { return writer_; }
    void finish() { close(); require(!failed_, "pipe drain failed"); }
private:
    void close() {
        if (writer_ >= 0) ::close(std::exchange(writer_, -1));
        if (thread_.joinable()) thread_.join();
    }
    int reader_ = -1, writer_ = -1;
    bool failed_ = false; // read only after join
    std::thread thread_;
};

pf_report_sink_summary_v1 summary(BacktestEngine& host) {
    pf_report_sink_summary_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_report_sink_summary_v1(handle(host), &result) == 0,
            "summary query refused");
    return result;
}
pf_report_sink_receipt_v1 receipt(BacktestEngine& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_report_sink_receipt_v1(handle(host), &result) == 0,
            "trailer receipt query refused");
    return result;
}
pf_report_sink_diagnostics_v1 diagnostics(BacktestEngine& host) {
    pf_report_sink_diagnostics_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_report_sink_diagnostics_v1(handle(host), &result) == 0,
            "sink diagnostics query refused");
    return result;
}
void equal_diagnostics(BacktestEngine& host, const ReportC& array) {
    const auto got = diagnostics(host);
    require(got.complete == 1 && got.count_e == array.equity_curve_len,
            "sink diagnostics lost the presented equity count");
    if (got.count_e) {
        const auto equal_point = [](const auto& left, const auto& right) {
            return left.time_ms == right.time_ms && same(left.equity, right.equity)
                && same(left.open_profit, right.open_profit);
        };
        require(equal_point(got.first, array.equity_curve[0])
                && equal_point(got.last, array.equity_curve[array.equity_curve_len - 1]),
                "sink diagnostics differ from the independent array endpoints");
    }
    bool trade_at_start = false;
    for (int i = 0; i < array.trades_len; ++i)
        trade_at_start |= array.trades[i].entry_bar_index <= 0;
    require(got.trade_at_start == static_cast<std::uint32_t>(trade_at_start),
            "sink diagnostics lost the report's first-bar trade provenance");
    const auto repeated = diagnostics(host);
    require(std::memcmp(&got, &repeated, sizeof got) == 0,
            "reading sink diagnostics changed their published generation");
}
void install_path(BacktestEngine& host, const std::string& path) {
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L22Q", nullptr};
    require(strategy_set_report_sink_v1(handle(host), &sink) == 0, "path sink refused");
}
void install_fd(BacktestEngine& host, int fd) {
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, fd, "L22Q"};
    require(strategy_set_report_sink_fd_v1(handle(host), &sink) == 0, "fd sink refused");
}

// Independent definition: first extrema from STL over the report array, wins
// and losses from predicates, drawdown from an explicit ordered prefix array.
// No engine accumulator or receipt is used to construct expected values.
pf_report_sink_summary_v1 reference(const ReportC& report) {
    require(report.trades_len == report.total_trades, "reference is not an array report");
    pf_report_sink_summary_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    result.complete = 1;
    std::vector<double> pnls;
    for (int row = 0; row < report.trades_len; ++row) {
        const auto& trade = report.trades[row];
        pnls.push_back(trade.pnl);
        const std::array<double, 8> fields{{trade.entry_price, trade.exit_price, trade.qty,
            trade.pnl, trade.pnl_pct, trade.max_runup, trade.max_drawdown, trade.commission}};
        const auto invalid = std::find_if(fields.begin(), fields.end(),
            [](double value) { return !std::isfinite(value); });
        if (!result.nonfinite_t_field && invalid != fields.end())
            result.nonfinite_t_field = static_cast<std::int32_t>(invalid - fields.begin()) + 5;
    }
    result.count_t = static_cast<std::int64_t>(pnls.size());
    result.wins = std::count_if(pnls.begin(), pnls.end(), [](double pnl) { return pnl > 0; });
    result.losses = std::count_if(pnls.begin(), pnls.end(), [](double pnl) { return pnl < 0; });
    if (!pnls.empty()) {
        result.best_trade = *std::max_element(pnls.begin(), pnls.end());
        result.worst_trade = *std::min_element(pnls.begin(), pnls.end());
    }
    pnls.insert(pnls.begin(), 0.0); // Python starts the cumulative fold at +0.0
    std::partial_sum(pnls.begin(), pnls.end(), pnls.begin());
    double peak = 0.0;
    for (auto it = pnls.begin() + 1; it != pnls.end(); ++it) {
        peak = std::max(peak, *it);
        result.max_drawdown = std::min(result.max_drawdown, *it - peak);
    }
    return result;
}
void equal(const pf_report_sink_summary_v1& actual, const pf_report_sink_summary_v1& expected,
           const std::string& where) {
    require(actual.count_t == expected.count_t && actual.wins == expected.wins
            && actual.losses == expected.losses, where + ": T/win/loss counts differ");
    require(same(actual.best_trade, expected.best_trade), where + ": best P&L bits differ");
    require(same(actual.worst_trade, expected.worst_trade), where + ": worst P&L bits differ");
    require(same(actual.max_drawdown, expected.max_drawdown), where + ": drawdown bits differ");
    require(actual.nonfinite_t_field == expected.nonfinite_t_field,
            where + ": first nonfinite field differs");
    require(std::memcmp(&actual, &expected, sizeof actual) == 0,
            where + ": receipt metadata or representation differs");
}

enum class Case { sealed, wins, losses, empty };
struct Options {
    Case kind = Case::sealed;
    bool selected = false;
    bool observed = false;
    bool quote = false;
};

// Same 146-bar tape, two entries and six-bar close cadence as L8c's
// sealed_sink_capture_and_read fixture, including its resident and open tails.
std::vector<Bar> tape(int count, Case kind = Case::sealed) {
    std::vector<Bar> bars;
    for (int index = 0; index < count; ++index) {
        const bool monotonic = kind == Case::wins || kind == Case::losses;
        const double price = monotonic ? 100.0 + index * 0.25
            : 100.0 + (index % 13 - 6) * 0.25;
        bars.push_back({price, price + 1.5, price - 1.5,
            price + (monotonic || index % 2 == 0 ? 0.25 : -0.25),
            1.0, start_ms + index * minute});
    }
    return bars;
}
class PineFixture final : public source::PineStrategyHost {
public:
    explicit PineFixture(Options options, bool streamable = true) : options_(options) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.25;
        config.pyramiding = 3;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = options.kind == Case::sealed ? 0.125 : 0.0;
        configure_pine_strategy(config);
        set_trade_history_streamable(streamable);
        set_broker_state_hash_recording(true);
    }
    const TradeLedger& ledger() const { return trades_; }
    std::vector<std::uint64_t> hashes;
    void execute(const std::vector<Bar>& bars) {
        commanded_ = -1;
        hashes.clear();
        run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    }
    void on_source_bar(const Bar&) override {
        hashes.push_back(broker_state_hash());
        const int index = pine_bar_index();
        if (commanded_ == index) return;
        commanded_ = index;
        if (options_.kind == Case::empty) return;
        const bool long_side = options_.kind == Case::wins
            || (options_.kind == Case::sealed && (index / 6) % 2 == 0);
        if (index % 6 == 0) {
            strategy_entry("entry-a", long_side, missing, missing, 1.25, "a");
            strategy_entry("entry-b", long_side, missing, missing, 2.5, "b");
        }
        if (index % 6 == 3) strategy_close_all();
    }
private:
    Options options_;
    int commanded_ = -1;
};
void configure_capture(BacktestEngine& host, const Options& options, int count) {
    if (options.observed) {
        const pf_execution_observer_v1 observer{sizeof observer, 1, nullptr,
            [](void*, const pf_execution_boundary_v1*, pf_boundary_receipt_v1* out) -> int {
                out->frame_bytes = 0;
                return 0;
            }};
        require(strategy_set_execution_observer_v1(handle(host), &observer) == 0,
                "observer refused");
    }
    if (options.selected) {
        const pf_selected_window_config_v1 window{sizeof window, 1, start_ms,
            start_ms + (count + 1) * minute};
        require(strategy_set_selected_window_v1(handle(host), &window) == 0,
                "selected window refused");
    }
}

// Real kernel runs with live lots. The supported presentation hook edits
// the range-end report rows only. This supplies exact signed zeros/nonfinite
// report doubles without corrupting executable state or touching the ledger.
class PresentedFixture final : public NativeStrategyHost {
public:
    explicit PresentedFixture(std::vector<double> pnls, int invalid_field = 0)
        : pnls_(std::move(pnls)), invalid_field_(invalid_field) {
        NativeRunSpec spec;
        spec.identity = {"L22Q-presentation", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:L22Q";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        spec.report_open_position_at_end = true;
        require(configure_native(spec).status == NativeSetupStatus::Applied,
                "presentation fixture configure refused");
        set_trade_history_streamable(true);
    }
    bool abort_run = false;
    void on_native_run_begin() override { callbacks_ = 0; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (static_cast<std::size_t>(callbacks_++) < pnls_.size()) {
            const auto placed = submit({native_order::Transact{1.0}, "open", ""});
            require(placed.status == native_order::SubmitStatus::Accepted, "opening refused");
        }
        if (abort_run && callbacks_ == 2) request_abort();
    }
    void execute(const std::vector<Bar>& bars) {
        run(bars.data(), static_cast<int>(bars.size()));
    }
protected:
    void present_report(ReportC* report) const override {
        require(report->trades_len == static_cast<int>(pnls_.size()),
                "presentation fixture lacks its real open lots");
        for (int row = 0; row < report->trades_len; ++row) {
            require(report->trades[row].open_at_end != 0, "presentation touched a closed row");
            report->trades[row].pnl = pnls_[static_cast<std::size_t>(row)];
        }
        if (invalid_field_) {
            auto field = [](TradeC& row, int index) -> double& {
                switch (index) {
                case 5: return row.entry_price;
                case 6: return row.exit_price;
                case 7: return row.qty;
                case 8: return row.pnl;
                case 9: return row.pnl_pct;
                case 10: return row.max_runup;
                case 11: return row.max_drawdown;
                case 12: return row.commission;
                default: throw std::runtime_error("bad test field index");
                }
            };
            field(report->trades[0], invalid_field_) = std::numeric_limits<double>::infinity();
            // Two bad fields in row zero and a lower index in row one:
            // row order must take precedence over the later field number.
            report->trades[0].commission = missing;
            report->trades[1].entry_price = missing;
        }
    }
private:
    std::vector<double> pnls_;
    int invalid_field_;
    int callbacks_ = 0;
};

template<class Host>
void run(Host& host, const std::vector<Bar>& bars) {
    host.execute(bars);
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "engine run failed: " + host.last_error());
}
void repeated_read(BacktestEngine& host, const pf_report_sink_summary_v1& expected,
                   const std::string& where) {
    const auto before = receipt(host);
    Report again;
    host.fill_report(&again.value);
    equal(summary(host), expected, where + " repeat capture");
    equal(summary(host), expected, where + " repeat query");
    const auto after = receipt(host);
    require(std::memcmp(&before, &after, sizeof before) == 0,
            where + ": repeated reads advanced the T cursor/trailer");
}
void unpublished(BacktestEngine& host) {
    pf_report_sink_summary_v1 empty{};
    empty.struct_size = sizeof empty;
    empty.version = 1;
    equal(summary(host), empty, "incomplete summary");
    const auto endpoints = diagnostics(host);
    require(endpoints.complete == 0 && endpoints.count_e == 0
            && endpoints.trade_at_start == 0,
            "unpublished sink exposed annualization provenance");
}

void check_fold_copy(BacktestEngine& host, const ReportC& array,
                     const pf_report_sink_summary_v1& expected) {
    std::vector<TradeC> open_rows;
    for (int index = 0; index < array.trades_len; ++index)
        if (array.trades[index].open_at_end) open_rows.push_back(array.trades[index]);
    const auto& consumer = NativeExecutionConsumer::bound(host);
    for (int read = 0; read < 2; ++read) {
        auto folded = consumer.finish_report_sink_summary(host, open_rows.data(),
            static_cast<int>(open_rows.size()));
        // The internal read returns just folds; publication owns this prefix
        // and completion metadata. Re-reading must also work after publishing:
        // mutating the sealed accumulator at first fill would fail here.
        folded.struct_size = sizeof folded;
        folded.version = 1;
        folded.complete = 1;
        equal(folded, expected, "sealed-prefix copy");
    }
}

pf_report_sink_summary_v1 pine_case(Directory& directory, Options options, int count = 146) {
    std::fprintf(stderr, "L22Q Pine: bars=%d case=%d selected=%d observed=%d quote=%d\n",
        count, static_cast<int>(options.kind), options.selected, options.observed, options.quote);
    const auto bars = tape(count, options.kind);
    PineFixture baseline(options), path(options), fd(options), retained(options, false);
    Pipe pipe;
    install_path(path, directory.file());
    install_fd(fd, pipe.fd());
    install_path(retained, directory.file());
    for (auto* host : {&baseline, &path, &fd, &retained}) {
        configure_capture(*host, options, count);
        if (options.quote)
            require(host->set_report_terminal_quote(
                start_ms + (count / 5 * 5) * minute, 117.25), "terminal quote refused");
        run(*host, bars);
        unpublished(*host);
    }
    require(baseline.hashes == path.hashes && baseline.hashes == fd.hashes
            && baseline.hashes == retained.hashes, "report folds changed broker-state hashes");
    const auto base = path.ledger().base();
    if (options.kind != Case::empty && count == 146)
        require(base > 0 && base < path.ledger().size() && fd.ledger().base() == base,
                "146-bar fixture lacks sealed prefix plus resident suffix");
    require(retained.ledger().base() == 0, "non-streamable module sealed rows");
    Report expected;
    baseline.fill_report(&expected.value);
    const auto ref = reference(expected.value);
    if (options.kind != Case::empty) {
        require(ref.count_t > 0, "fixture has no trades");
        require(std::any_of(expected.value.trades,
            expected.value.trades + expected.value.trades_len,
            [](const TradeC& row) { return row.open_at_end != 0; }),
            "fixture has no presented open rows");
    }
    if (options.kind == Case::wins)
        require(ref.wins == ref.count_t && ref.worst_trade > 0, "all-win fixture is not all wins");
    if (options.kind == Case::losses)
        require(ref.losses == ref.count_t && ref.best_trade < 0, "all-loss fixture is not all losses");
    for (auto* host : {&path, &fd, &retained}) {
        check_fold_copy(*host, expected.value, ref);
        unpublished(*host);
        Report actual;
        host->fill_report(&actual.value);
        const auto got = summary(*host);
        equal(got, ref, "array reference");
        equal_diagnostics(*host, expected.value);
        const auto trailer = receipt(*host);
        require(got.complete == trailer.complete && got.count_t == trailer.count_t,
                "summary and trailer disagree");
        require(actual.value.total_trades == expected.value.total_trades
                && same(actual.value.net_profit, expected.value.net_profit)
                && actual.value.input_bars_processed == expected.value.input_bars_processed,
                "summary document's engine scalar sources changed");
        require(actual.value.trades == nullptr && actual.value.trades_len == 0,
                "sink exposed a report trade array");
        repeated_read(*host, got, "Pine route");
        check_fold_copy(*host, expected.value, ref);
    }
    equal(summary(fd), summary(path), "fd/path summary");
    equal(summary(retained), summary(path), "non-streamable/streamable summary");
    require(path.ledger().base() == base, "capture advanced the sealed prefix");
    pipe.finish();
    return summary(path);
}

void presented_case(Directory& directory, const std::vector<double>& pnls, int invalid = 0,
                    bool capture = false) {
    std::fprintf(stderr, "L22Q presented: rows=%zu invalid=%d capture=%d first_sign=%d\n",
        pnls.size(), invalid, capture, std::signbit(pnls.front()));
    const auto bars = tape(static_cast<int>(pnls.size()) + 3);
    PresentedFixture baseline(pnls, invalid), path(pnls, invalid), fd(pnls, invalid);
    Pipe pipe;
    install_path(path, directory.file());
    install_fd(fd, pipe.fd());
    Options options;
    options.selected = options.observed = capture;
    if (invalid) {
        // Canonical nonfinite rows fail before a summary can be published.
        const std::array<const char*, 8> fields{{"entry_price", "exit_price", "qty", "pnl",
            "pnl_pct", "max_runup", "max_drawdown", "commission"}};
        const std::string expected_args = std::string("{\"field\":\"trades.")
            + fields.at(static_cast<std::size_t>(invalid - 5)) + "\",\"reason\":\"nonfinite\"}";
        for (auto* host : {&baseline, &path, &fd}) {
            configure_capture(*host, options, static_cast<int>(bars.size()));
            bool refused = false;
            RunFailureValue failure{};
            try {
                host->execute(bars);
                if (host->last_run_status() != 0 || !host->last_error().empty()) {
                    refused = true;
                    failure = run_failure_value_of(*host);
                } else {
                    Report report;
                    host->fill_report(&report.value);
                }
            } catch (const std::exception& error) {
                refused = true;
                failure = classify_run_failure(error);
            }
            require(refused, "nonfinite presentation was accepted");
            require(failure.code == RunFailureCode::report_result_invalid
                    && failure.args && *failure.args == expected_args,
                    "nonfinite presentation lost its first-field typed refusal");
            unpublished(*host);
            require(receipt(*host).complete == 0, "nonfinite presentation completed a sink");
        }
        pipe.finish();
        return;
    }
    for (auto* host : {&baseline, &path, &fd}) {
        configure_capture(*host, options, static_cast<int>(bars.size()));
        run(*host, bars);
        unpublished(*host);
    }
    Report array;
    baseline.fill_report(&array.value);
    const auto ref = reference(array.value);
    if (!invalid && std::all_of(pnls.begin(), pnls.end(), [](double pnl) { return pnl == 0; })) {
        require(ref.wins == 0 && ref.losses == 0, "zero-only report counted a win/loss");
        require(std::signbit(array.value.trades[0].pnl) == std::signbit(pnls.front()),
                "presentation lost the first zero's sign before folding");
    }
    for (auto* host : {&path, &fd}) {
        Report actual;
        host->fill_report(&actual.value);
        equal(summary(*host), ref, "presented array reference");
        equal_diagnostics(*host, array.value);
        repeated_read(*host, ref, "presented route");
    }
    pipe.finish();
}

void lifecycle(Directory& directory) {
    PineFixture host(Options{});
    unpublished(host); // no sink; never binds a consumer merely to query
    require(strategy_report_sink_summary_v1(handle(host), nullptr) == -1, "NULL out accepted");
    require(strategy_report_sink_diagnostics_v1(handle(host), nullptr) == -1,
            "NULL diagnostics accepted");
    pf_report_sink_diagnostics_v1 bad_diagnostics;
    std::memset(&bad_diagnostics, 0xa5, sizeof bad_diagnostics);
    bad_diagnostics.struct_size = sizeof bad_diagnostics;
    bad_diagnostics.version = 1;
    const auto original_diagnostics = bad_diagnostics;
    require(strategy_report_sink_diagnostics_v1(nullptr, &bad_diagnostics) == -1
            && std::memcmp(&bad_diagnostics, &original_diagnostics, sizeof bad_diagnostics) == 0,
            "bad diagnostics handle rewrote output");
    for (const bool bad_version : {false, true}) {
        bad_diagnostics = original_diagnostics;
        if (bad_version) bad_diagnostics.version = 2;
        else --bad_diagnostics.struct_size;
        const auto before = bad_diagnostics;
        require(strategy_report_sink_diagnostics_v1(handle(host), &bad_diagnostics) == -1
                && std::memcmp(&bad_diagnostics, &before, sizeof before) == 0,
                "bad diagnostics prefix rewrote output");
    }
    pf_report_sink_summary_v1 bad;
    std::memset(&bad, 0xa5, sizeof bad);
    bad.struct_size = sizeof bad;
    bad.version = 1;
    const auto original = bad;
    require(strategy_report_sink_summary_v1(nullptr, &bad) == -1
            && std::memcmp(&bad, &original, sizeof bad) == 0, "bad handle rewrote output");
    for (const bool bad_version : {false, true}) {
        bad = original;
        if (bad_version) bad.version = 2;
        else --bad.struct_size;
        const auto before = bad;
        require(strategy_report_sink_summary_v1(handle(host), &bad) == -1
                && std::memcmp(&bad, &before, sizeof bad) == 0, "bad prefix rewrote output");
    }
    install_path(host, directory.file());
    require(strategy_set_report_sink_v1(handle(host), nullptr) == 0,
            "unstarted path sink removal refused");
    unpublished(host);
    install_path(host, directory.file());
    const auto bars = tape(146);
    pf_report_sink_summary_v1 first{};
    for (int attempt = 0; attempt < 2; ++attempt) {
        run(host, bars);
        unpublished(host); // a new run resets the prior completed summary
        Report report;
        host.fill_report(&report.value);
        if (attempt == 0) first = summary(host);
        else equal(summary(host), first, "reused handle");
    }
    // An ordinary run still owns its scratch curve; S's detach rule is fd-only.
    const auto before = receipt(host);
    const auto before_diagnostics = diagnostics(host);
    require(strategy_set_report_sink_v1(handle(host), nullptr) == -3,
            "ordinary path sink removal lost its -3 refusal");
    repeated_read(host, first, "refused ordinary path removal");
    const auto after = receipt(host);
    const auto after_diagnostics = diagnostics(host);
    require(std::memcmp(&before, &after, sizeof before) == 0,
            "refused removal changed the completed receipt");
    require(std::memcmp(&before_diagnostics, &after_diagnostics, sizeof before_diagnostics) == 0,
            "refused removal changed the completed diagnostics");

    // Exercise failure after a real begin: no trailer means no published fold.
    PresentedFixture failed({-0.0, +0.0});
    install_path(failed, directory.path + "/missing/report.ndjson");
    try { failed.execute(tape(6)); } catch (const std::exception&) {}
    require(failed.last_run_status() != 0 || !failed.last_error().empty(),
            "bad sink path did not fail the run");
    unpublished(failed);
    PresentedFixture aborted({-0.0, +0.0});
    Pipe pipe;
    install_fd(aborted, pipe.fd());
    aborted.abort_run = true;
    aborted.execute(tape(6));
    require(aborted.last_run_status() != 0 || !aborted.last_error().empty(),
            "abort did not stop the run");
    unpublished(aborted);
    require(receipt(aborted).complete == 0, "aborted run acquired a trailer");
    pipe.finish();
}
} // namespace

int main() {
    try {
        Directory directory;
        pine_case(directory, Options{}); // L8c 146: sealed + resident + open
        pine_case(directory, Options{}, 14); // ordinary short run
        for (const Case kind : {Case::wins, Case::losses, Case::empty}) {
            Options options;
            options.kind = kind;
            pine_case(directory, options);
        }
        Options quoted;
        quoted.quote = true;
        const auto ordinary_quote = pine_case(directory, quoted);
        for (const bool selected : {false, true}) {
            Options options;
            options.selected = selected;
            options.observed = true;
            options.quote = true; // range-end P&L must be folded AFTER presentation
            equal(pine_case(directory, options), ordinary_quote, "observed/ordinary route");
        }
        Options selected_only;
        selected_only.selected = selected_only.quote = true;
        equal(pine_case(directory, selected_only), ordinary_quote, "selected/ordinary route");
        for (const bool capture : {false, true}) {
            presented_case(directory, {-0.0, +0.0}, 0, capture);
            presented_case(directory, {+0.0, -0.0}, 0, capture);
        }
        presented_case(directory, {1e16, 1.0, -1e16, -2.0}); // ordered cancellation
        for (int field = 5; field <= 12; ++field)
            presented_case(directory, {1.0, -2.0}, field, field % 2 == 0);
        lifecycle(directory);
        std::puts("test_report_sink_summary: differential receipt checks passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_sink_summary: %s\n", error.what());
        return 1;
    }
}
