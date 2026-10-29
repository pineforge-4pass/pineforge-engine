// Compile the real metric/report TUs with observation only. The accumulator
// body comes from the build-directory overlay, including same-TU array calls.
#include "closed_trade_query_work.hpp"
#include "trade_stats_reference.hpp"

// TradeLedger's declaration is already included. Only call arguments in the
// real report TU are forwarded; this counts hits as well as scratch reads.
// The report TU comes first: the metrics TU declares its own metrics::detail,
// and once that is visible the report TU's `detail::` names inside functions
// that use `metrics` become ambiguous with pineforge::detail.
#define numeric_at(index) numeric_at(::closed_trade_query_work::numeric_index(__func__, (index)))
#include "../src/engine_report.cpp"
#undef numeric_at

#include "closed_trade_work_engine_metrics.cpp"

#ifndef PINEFORGE_TEST_CLOSED_TRADE_METRIC_OVERLAY
#error "setup failure: accumulator input counting overlay is missing"
#endif

namespace {
using namespace closed_trade_query_work;
namespace orders = pineforge::native_order;

NativeRunSpec specification() {
    NativeRunSpec spec;
    spec.identity = {"closed-trade-statistics", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = initial_capital;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.125;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = false;
    return spec;
}

class ReportHost final : public NativeStrategyHost {
public:
    explicit ReportHost(ReportMode mode, int terminal_after = -1)
        : terminal_after_(terminal_after) {
        require(set_report_mode_v1(mode) == PF_REPORT_MODE_ACCEPTED,
                "setup failure: report mode refused");
        auto spec = specification();
        spec.report_open_position_at_end = terminal_after >= 0;
        require(configure_native(spec).status == NativeSetupStatus::Applied,
                "setup failure: native specification refused");
    }

    void on_native_bar(const Bar&, const NativeDecisionContext& context) override {
        const int index = context.coordinate.interval_index;
        const int trade = index / 4;
        if (index % 4 == 0) {
            const double units = (long_side(trade) ? 1.0 : -1.0) * quantity(trade);
            accepted(submit({orders::Transact{units}, "position", ""}));
        } else if (index % 4 == 1 && trade != terminal_after_) {
            accepted(submit({orders::Flatten{}, "close", ""}));
        }
    }

    TradeLedger& rows() { return trades_; }
    const TradeLedger& rows() const { return trades_; }

    void prepare_next_run() {
        auto spec = specification();
        spec.identity.run_number = ++run_number_;
        spec.report_open_position_at_end = terminal_after_ >= 0;
        require(configure_native(spec).status == NativeSetupStatus::Applied,
                "setup failure: reused native specification refused");
    }

private:
    int terminal_after_;
    std::uint64_t run_number_ = 1;
    template<class Result>
    static void accepted(const Result& result) {
        require(result.status == orders::SubmitStatus::Accepted && result.handle.has_value(),
                "setup failure: real entry/close command refused");
    }
};

struct OwnedReport {
    ReportC value{};
    ~OwnedReport() { BacktestEngine::free_report(&value); }
};

struct OwnedEnvelope {
    ReportEnvelopeV1 value{};
    OwnedEnvelope() {
        value.metadata.struct_size = sizeof(pf_report_metadata_v1);
        value.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    }
    ~OwnedEnvelope() { BacktestEngine::free_report(&value.report); }
};

struct Oracle {
    pf_trade_stats_t all{}, longs{}, shorts{};
    double net = 0.0;
};

Oracle oracle(const std::vector<TradeC>& rows) {
    Oracle result;
    const int count = static_cast<int>(rows.size());
    result.all = trade_stats_reference::compute_trade_stats(
        rows.data(), count, metrics::TradeFilter::ALL, initial_capital);
    result.longs = trade_stats_reference::compute_trade_stats(
        rows.data(), count, metrics::TradeFilter::LONG, initial_capital);
    result.shorts = trade_stats_reference::compute_trade_stats(
        rows.data(), count, metrics::TradeFilter::SHORT, initial_capital);
    double arithmetic_net = 0.0;
    for (int index = 0; index < count; ++index) {
        result.net += rows[static_cast<std::size_t>(index)].pnl;
        arithmetic_net += expected_pnl(index);
    }
    equal_double(result.net, arithmetic_net, "ordered report net / hand arithmetic");
    equal_double(result.all.net_profit, result.net, "all-trade oracle net");
    require(result.all.num_trades == count && result.longs.num_trades == count / 2
                && result.shorts.num_trades == count / 2,
            "value mismatch: all/long/short oracle populations");
    for (const auto* block : {&result.all, &result.longs, &result.shorts})
        require(block->num_wins > 0 && block->num_losses > 0 && block->num_even > 0
                    && block->num_trades == block->num_wins + block->num_losses + block->num_even,
                "value mismatch: oracle lacks winners, losers or evens");
    return result;
}

void equal_report(const ReportC& report, const Oracle& expected, int trades) {
    require(report.total_trades == trades, "value mismatch: report logical population");
    equal_double(report.net_profit, expected.net, "report.net_profit");
    equal_stats(report.metrics.all, expected.all);
    equal_stats(report.metrics.longs, expected.longs);
    equal_stats(report.metrics.shorts, expected.shorts);
}

void forwarding_control(const std::vector<Bar>& bars, const Oracle& expected, int trades) {
    ReportHost host(ReportMode::FULL);
    host.run(bars.data(), static_cast<int>(bars.size()));
    check_completed(host);
    const auto rows = public_rows(host, trades);
    require(rows.size() == static_cast<std::size_t>(trades),
            "setup failure: forwarding run is empty");
    OwnedReport report;
    Work observed{};
    {
        CountScope scope(observed);
        host.fill_report(&report.value);
    }
    equal_report(report.value, expected, trades);
    require(report.value.trades != nullptr && report.value.trades_len == trades,
            "setup failure: FULL forwarding control omitted its real rows");
    // FULL must output the rows even when the ledger cache is warm.
    require(observed.net_rows == static_cast<std::uint64_t>(trades)
                && observed.net_rows != 0,
            "setup failure: report forwarding control observed no numeric row visits");

    // Exercise the public array helper explicitly: a later report cache may
    // correctly stop calling it, while this caller-owned array still needs
    // its own real fold. The counter must see even same-TU/inlined calls.
    Work array_work{};
    std::array<pf_trade_stats_t, 3> actual{};
    {
        CountScope scope(array_work);
        actual[0] = metrics::compute_trade_stats(
            rows.data(), trades, metrics::TradeFilter::ALL, initial_capital);
        actual[1] = metrics::compute_trade_stats(
            rows.data(), trades, metrics::TradeFilter::LONG, initial_capital);
        actual[2] = metrics::compute_trade_stats(
            rows.data(), trades, metrics::TradeFilter::SHORT, initial_capital);
    }
    equal_stats(actual[0], expected.all);
    equal_stats(actual[1], expected.longs);
    equal_stats(actual[2], expected.shorts);
    require(array_work.accumulator_inputs == 3 * static_cast<std::uint64_t>(trades)
                && array_work.accumulator_inputs != 0,
            "setup failure: real array metric control observed no accumulator inputs");
    std::printf("CONTROL VERIFIED closed=%d numeric-visits=%llu accumulator-inputs=%llu\n",
                trades, static_cast<unsigned long long>(observed.net_rows),
                static_cast<unsigned long long>(array_work.accumulator_inputs));
}

struct Sample {
    Work execution{};
    std::array<Work, query_count> reports{};
};

Sample sample(int trades) {
    const auto bars = tape(trades);
    ReportHost host(ReportMode::SUMMARY_ONLY);
    Sample result;
    {
        CountScope scope(result.execution, host.rows());
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    check_completed(host);
    const auto rows = public_rows(host, trades);
    const auto expected = oracle(rows);
    forwarding_control(bars, expected, trades);

    for (std::size_t query = 0; query < query_count; ++query) {
        OwnedEnvelope envelope;
        int status = 0;
        {
            CountScope scope(result.reports[query], host.rows());
            // Outside dispatch, with no execution observer, selected window
            // or sink: the ordinary ledger metric route must answer.
            status = host.fill_report_envelope_v1(&envelope.value);
        }
        require(status == PF_REPORT_MODE_ACCEPTED,
                "value mismatch: report read returned an error status");
        check_completed(host);
        const auto& report = envelope.value.report;
        equal_report(report, expected, trades);
        require(report.trades == nullptr && report.trades_len == 0
                    && report.equity_curve == nullptr && report.equity_curve_len == 0,
                "value mismatch: SUMMARY_ONLY materialized an output array");
        require(envelope.value.metadata.mode == PF_REPORT_MODE_SUMMARY_ONLY
                    && envelope.value.metadata.trades.logical_count == static_cast<std::uint64_t>(trades)
                    && envelope.value.metadata.trades.captured == 0
                    && envelope.value.metadata.trades.external == 0,
                "value mismatch: summary metadata or capture population");
        const auto& work = result.reports[query];
        std::printf("VALUES VERIFIED closed=%d query=%zu net=%a "
                    "numeric-net=%llu numeric-metrics=%llu other-numeric=%llu "
                    "fold-inputs=%llu cached-fold-rows=%llu\n",
                    trades, query, report.net_profit,
                    static_cast<unsigned long long>(work.net_rows),
                    static_cast<unsigned long long>(work.metric_rows),
                    static_cast<unsigned long long>(work.other_numeric_rows),
                    static_cast<unsigned long long>(work.accumulator_inputs),
                    static_cast<unsigned long long>(work.report_fold_rows));
    }
    return result;
}

Oracle current_oracle(const BacktestEngine& engine) {
    std::vector<TradeC> rows;
    for (std::size_t index = 0; index < engine.closed_trade_count(); ++index)
        rows.push_back(public_report_row(engine.closed_trade(index)));
    for (int index = static_cast<int>(engine.closed_trade_count());
         index < engine.report_trade_count(); ++index)
        rows.push_back(public_report_row(engine.get_report_trade(index)));
    Oracle result;
    const auto count = static_cast<int>(rows.size());
    result.all = trade_stats_reference::compute_trade_stats(
        rows.data(), count, metrics::TradeFilter::ALL, initial_capital);
    result.longs = trade_stats_reference::compute_trade_stats(
        rows.data(), count, metrics::TradeFilter::LONG, initial_capital);
    result.shorts = trade_stats_reference::compute_trade_stats(
        rows.data(), count, metrics::TradeFilter::SHORT, initial_capital);
    for (const auto& row : rows) result.net += row.pnl;
    return result;
}

void no_closed_work(const Work& work, const char* context) {
    require(work.net_rows == 0 && work.metric_rows == 0 && work.other_numeric_rows == 0
                && work.accumulator_inputs == 0 && work.report_fold_rows == 0,
            context);
}

template<class Engine>
Work current_report(const Engine& engine, const char* context, const Oracle* snapshot = nullptr) {
    const auto expected = snapshot ? *snapshot : current_oracle(engine);
    Work observed{};
    try {
        OwnedEnvelope envelope;
        {
            CountScope scope(observed, engine.rows());
            require(engine.fill_report_envelope_v1(&envelope.value) == PF_REPORT_MODE_ACCEPTED,
                    "value mismatch: amended report read refused");
        }
        equal_report(envelope.value.report, expected, engine.report_trade_count());
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(context) + ": " + error.what());
    }
    return observed;
}

void run_population(ReportHost& host, int count) {
    const auto bars = tape(count);
    host.run(bars.data(), static_cast<int>(bars.size()));
    check_completed(host);
    public_rows(host, count);
    current_report(host, "real run before amendment");
}

void exact_rebuild(const Work& work, std::size_t rows, const char* context) {
    // Each row is validated/net-folded and feeds the three independent
    // accumulators once. These counts are layout estimates from source,
    // not measured here; the lab executes these equalities.
    require(work.report_fold_rows == rows, context);
    require(work.net_rows == 0 && work.metric_rows == 0 && work.other_numeric_rows == 0,
            "cache rebuild also replayed the old report loops");
}

void old_row_amendments() {
    ReportHost host(ReportMode::SUMMARY_ONLY);
    run_population(host, large_trades);
    require(host.rows().resident_base() > 1,
            "setup failure: amendment must precede the descriptor window");
    host.rows().modify(0, [](Trade& row) {
        row.pnl *= 2.0;
        row.pnl_pct *= 2.0;
        row.commission = 0.125;
    });
    const auto hash = host.broker_state_hash();
    const auto amended = current_report(host, "old closed row amended after report");
    exact_rebuild(amended, host.rows().size() - host.rows().base(),
                  "old-row amendment did not rebuild exactly the covered suffix");
    no_closed_work(current_report(host, "quiet report after amendment"),
                   "quiet amended report consumed closed rows");
    require(host.broker_state_hash() == hash, "report cache changed the broker state hash");
}

void truncate_and_reappend() {
    ReportHost host(ReportMode::SUMMARY_ONLY);
    run_population(host, large_trades);
    const auto end = host.closed_trade_count();
    std::vector<Trade> tail;
    for (std::size_t index = end - 3; index < end; ++index)
        tail.push_back(host.closed_trade(index));
    host.rows().resize(end - tail.size());
    for (auto row : tail) {
        row.pnl *= 2.0;
        row.pnl_pct *= 2.0;
        host.rows().push_back(std::move(row));
    }
    exact_rebuild(current_report(host, "same-count truncate and reappend"), end,
                  "same-count replacement did not rebuild exactly the closed population");
    no_closed_work(current_report(host, "quiet report after truncate and reappend"),
                   "quiet recut report consumed closed rows");

    // Once rebuilt, a new append must be folded during publication, before
    // even its first report read. No row is fabricated by the fixture.
    Work appended{};
    {
        CountScope scope(appended, host.rows());
        host.rows().push_back(tail.front());
    }
    require(appended.report_fold_rows == 1, "appended row was not folded eagerly exactly once");
    no_closed_work(current_report(host, "first report after eager append"),
                   "first report after append consumed closed rows");
}

struct Failure {
    RunFailureValue value{};
    std::string message;
    int total = 0;
    double net = 0.0;
};

Failure report_failure(const BacktestEngine& engine) {
    OwnedReport report;
    try {
        engine.fill_report(&report.value);
    } catch (const std::exception& error) {
        return {classify_run_failure(error), error.what(),
                report.value.total_trades, report.value.net_profit};
    }
    throw std::runtime_error("invalid report unexpectedly succeeded");
}

void equal_failure(const Failure& actual, const Failure& expected) {
    require(actual.value.code == expected.value.code
                && ((!actual.value.args && !expected.value.args)
                    || (actual.value.args && expected.value.args
                        && *actual.value.args == *expected.value.args))
                && actual.message == expected.message,
            "cached report changed the first typed numeric error");
    require(actual.total == expected.total, "cached report changed partial row count");
    equal_double(actual.net, expected.net, "partial report net before numeric failure");
}

void invalid_numeric_rows() {
    ReportHost summary(ReportMode::SUMMARY_ONLY), full(ReportMode::FULL);
    run_population(summary, small_trades);
    run_population(full, small_trades);
    // Use the untouched FULL producer as the old fold's error oracle. The
    // earlier row's later field must win over the next row's earlier field.
    for (auto* host : {&summary, &full}) {
        host->rows().modify(1, [](Trade& row) {
            row.commission = std::numeric_limits<double>::infinity();
        });
        host->rows().modify(2, [](Trade& row) {
            row.entry_price = std::numeric_limits<double>::quiet_NaN();
        });
    }
    const auto expected = report_failure(full);
    Work invalid_rebuild{}, invalid_repeat{};
    {
        CountScope scope(invalid_rebuild, summary.rows());
        equal_failure(report_failure(summary), expected);
    }
    require(invalid_rebuild.report_fold_rows == 2,
            "invalid-row rebuild did not stop at the first invalid row");
    {
        CountScope scope(invalid_repeat, summary.rows());
        equal_failure(report_failure(summary), expected);
    }
    no_closed_work(invalid_repeat, "repeated invalid report consumed closed rows");

    // A valid cache must consume an invalid append too: deferring its
    // validation failure until the report preserves execution behavior.
    ReportHost append_summary(ReportMode::SUMMARY_ONLY), append_full(ReportMode::FULL);
    run_population(append_summary, small_trades);
    run_population(append_full, small_trades);
    auto row = append_summary.closed_trade(0);
    row.qty = std::numeric_limits<double>::infinity();
    Work invalid_append{};
    {
        CountScope scope(invalid_append, append_summary.rows());
        append_summary.rows().push_back(row);
    }
    require(invalid_append.report_fold_rows == 1,
            "invalid append skipped its eager fold or validation failure");
    append_full.rows().push_back(row);
    const auto appended_expected = report_failure(append_full);
    for (std::size_t query = 0; query < query_count; ++query) {
        Work work{};
        {
            CountScope scope(work, append_summary.rows());
            equal_failure(report_failure(append_summary), appended_expected);
        }
        no_closed_work(work, "report replayed an eagerly validated invalid row");
    }

    // Each finite term validates; the aggregate fails after publishing the
    // overflowing row's count, while retaining the preceding finite net.
    for (auto* host : {&summary, &full}) {
        host->rows().resize(2);
        for (std::size_t index = 0; index < host->rows().size(); ++index) {
            auto finite = host->closed_trade(0);
            finite.pnl = std::numeric_limits<double>::max();
            finite.commission = 0.0;
            host->rows().replace(index, finite);
        }
    }
    equal_failure(report_failure(summary), report_failure(full));

    // Finite rows/net can still overflow an accumulator. ALL must finish
    // before LONG and SHORT, preserving the old field and error arguments.
    for (auto* host : {&summary, &full}) {
        host->prepare_next_run();
        run_population(*host, small_trades);
        host->rows().modify(0, [](Trade& finite) {
            finite.commission = std::numeric_limits<double>::max();
        });
        host->rows().modify(1, [](Trade& finite) {
            finite.commission = std::numeric_limits<double>::max();
        });
    }
    const auto metric_expected = report_failure(full);
    equal_failure(report_failure(summary), metric_expected);
    equal_failure(report_failure(summary), metric_expected);
}

class ReportEngineCopy final : public BacktestEngine {
public:
    explicit ReportEngineCopy(const BacktestEngine& engine) : BacktestEngine(engine) {}
    void on_bar(const Bar&) override {
        throw std::logic_error("setup failure: report copy received dispatch");
    }
    TradeLedger& rows() { return trades_; }
    const TradeLedger& rows() const { return trades_; }
};

void copy_then_diverge() {
    ReportHost host(ReportMode::SUMMARY_ONLY);
    run_population(host, small_trades);
    ReportEngineCopy copy(host);
    current_report(copy, "copied report engine");
    const auto copied_before = current_oracle(copy);
    host.rows().modify(0, [](Trade& row) { row.pnl *= 2.0; row.pnl_pct *= 2.0; });
    current_report(host, "original report engine diverged");
    no_closed_work(current_report(copy, "copy retained original rows", &copied_before),
                   "quiet copied report replayed closed rows");
    const auto original_before = current_oracle(host);
    copy.rows().modify(1, [](Trade& row) { row.pnl *= 0.5; row.pnl_pct *= 0.5; });
    exact_rebuild(current_report(copy, "copied report engine diverged"), copy.rows().size(),
                  "diverged copy did not rebuild exactly its own rows");
    no_closed_work(current_report(copy, "second report from diverged copy"),
                   "second diverged-copy report replayed closed rows");
    no_closed_work(current_report(host, "original retained divergent rows", &original_before),
                   "copy mutation invalidated the original report cache");

    copy.rows() = host.rows();
    exact_rebuild(current_report(copy, "ledger copy assignment at unchanged count"),
                  host.rows().size(), "assigned ledger retained a stale report cache");
    TradeLedger moved(host.rows());
    copy.rows().modify(1, [](Trade& row) { row.pnl_pct *= 2.0; });
    current_report(copy, "warm destination before ledger move");
    copy.rows() = std::move(moved);
    exact_rebuild(current_report(copy, "ledger move assignment at unchanged count"),
                  host.rows().size(), "moved ledger retained the destination report cache");
}

void reseal_and_reset() {
    ReportHost host(ReportMode::SUMMARY_ONLY);
    run_population(host, small_trades);
    // No sink means a nonempty sealed prefix is refused by the public
    // report contract. Re-sealing the legal empty prefix still invalidates.
    host.rows().seal(0);
    exact_rebuild(current_report(host, "report after empty-prefix seal"), host.rows().size(),
                  "seal did not invalidate the report fold");
    host.rows().seal(0);
    exact_rebuild(current_report(host, "report after empty-prefix reseal"), host.rows().size(),
                  "reseal did not invalidate the report fold");
    no_closed_work(current_report(host, "quiet report after reseal"),
                   "quiet resealed report replayed closed rows");
    std::vector<Trade> saved;
    for (std::size_t index = 0; index < host.closed_trade_count(); ++index)
        saved.push_back(host.closed_trade(index));
    host.rows().clear();
    for (auto row : saved) {
        row.pnl *= 0.5;
        row.pnl_pct *= 0.5;
        host.rows().push_back(std::move(row));
    }
    exact_rebuild(current_report(host, "clear and repopulate at unchanged count"),
                  saved.size(), "clear retained a report cache from the previous rows");
    host.prepare_next_run();
    run_population(host, small_trades);
    no_closed_work(current_report(host, "report after run reset"),
                   "reset run failed to maintain the report fold eagerly");

    ReportHost full(ReportMode::FULL);
    run_population(full, small_trades);
    host.rows().seal(1);
    full.rows().seal(1);
    equal_failure(report_failure(host), report_failure(full));
    host.rows().seal(2);
    full.rows().seal(2);
    equal_failure(report_failure(host), report_failure(full));
}

void terminal_rows_are_transient() {
    ReportHost host(ReportMode::SUMMARY_ONLY, small_trades);
    const auto bars = tape(small_trades + 1);
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_run_status() == 0 && host.last_error().empty()
                && host.native_state().kind == NativeLifecycleKind::Completed,
            "setup failure: terminal-lot run failed");
    public_rows(host, small_trades);
    require(host.report_trade_count() > static_cast<int>(host.closed_trade_count())
                && host.get_report_trade(small_trades).open_at_end,
            "setup failure: terminal row is missing");
    for (std::size_t query = 0; query < query_count; ++query) {
        const auto work = current_report(host, "terminal row applied to report copy");
        require(work.report_fold_rows == 0 && work.net_rows == 0 && work.metric_rows == 0
                    && work.other_numeric_rows == 0,
                "terminal report consumed closed history");
        require(work.accumulator_inputs == 3,
                "terminal row must enter each copied accumulator exactly once");
    }
}

void replay_window_boundary() {
    constexpr std::size_t replay_rows = 16; // The contract's fixed repair window.
    ReportHost host(ReportMode::SUMMARY_ONLY);
    run_population(host, small_trades);
    require(host.rows().size() - host.rows().base() > replay_rows,
            "setup failure: repair boundary needs an older unsealed row");

    const auto inside_index = host.rows().size() - replay_rows;
    host.rows().modify(inside_index, [](Trade& row) {
        row.pnl += 0.25;
        row.pnl_pct += 0.125;
    });
    // The oldest row inside the window has a pre-row snapshot, so replay
    // consumes exactly the rows from its changed index to the current end.
    exact_rebuild(current_report(host, "amend oldest retained snapshot row"),
                  host.rows().size() - inside_index,
                  "oldest retained snapshot amendment did not replay exactly the changed suffix");

    const auto outside_index = host.rows().size() - replay_rows - 1;
    host.rows().modify(outside_index, [](Trade& row) {
        row.pnl += 0.25;
        row.pnl_pct += 0.125;
    });
    // One row farther back has no retained pre-row snapshot, so the read
    // must rebuild every unsealed row from the current base to the end.
    exact_rebuild(current_report(host, "amend row just outside snapshot window"),
                  host.rows().size() - host.rows().base(),
                  "outside-window amendment did not rebuild exactly the unsealed rows");

    const auto newest_index = host.rows().size() - 1;
    auto newest_row = host.closed_trade(newest_index);
    newest_row.pnl += 0.25;
    newest_row.pnl_pct += 0.125;
    host.rows().replace(newest_index, newest_row);
    // The newest covered row has its own pre-row snapshot, so only the
    // suffix from that changed index to the current end is consumed.
    exact_rebuild(current_report(host, "replace newest covered row"),
                  host.rows().size() - newest_index,
                  "newest-row replacement did not replay exactly the changed suffix");
    no_closed_work(current_report(host, "quiet report after boundary amendments"),
                   "quiet report after boundary amendments consumed closed rows");
}

} // namespace

int main() {
    try {
        const auto small = sample(small_trades);
        const auto large = sample(large_trades);
        bool replayed = false;
        for (std::size_t query = 0; query < query_count; ++query) {
            const auto& a = small.reports[query];
            const auto& b = large.reports[query];
            replayed = replayed || a.net_rows != 0 || b.net_rows != 0
                || a.metric_rows != 0 || b.metric_rows != 0
                || a.other_numeric_rows != 0 || b.other_numeric_rows != 0
                || a.accumulator_inputs != 0 || b.accumulator_inputs != 0
                || a.report_fold_rows != 0 || b.report_fold_rows != 0
                || b.net_rows > a.net_rows || b.metric_rows > a.metric_rows
                || b.accumulator_inputs > a.accumulator_inputs
                || b.report_fold_rows > a.report_fold_rows;
        }
        require(!replayed, "summary report replayed closed history");
        require(small.execution.report_fold_rows == small_trades
                    && large.execution.report_fold_rows == large_trades,
                "run did not eagerly fold every newly closed row exactly once");
        old_row_amendments();
        replay_window_boundary();
        truncate_and_reappend();
        invalid_numeric_rows();
        copy_then_diverge();
        reseal_and_reset();
        terminal_rows_are_transient();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_closed_trade_report_query_work: %s\n", error.what());
        return 1;
    }
}
