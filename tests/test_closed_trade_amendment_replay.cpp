// Work bounds below are layout estimates from source, not measured here.
// The test drives real Pine commands; mutations amend their public rows.
#include "closed_trade_query_work.hpp"
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <limits>

namespace {
using namespace closed_trade_query_work;

// Acceptance window, independent of the implementation and population sizes.
constexpr std::size_t replay_rows = 16;
// Per close, each of two folds consumes its new row and at most one window
// again. Multiple host annotations before the read coalesce at the first edit.
constexpr std::uint64_t work_per_close = 2 * (replay_rows + 1);

struct OwnedEnvelope {
    ReportEnvelopeV1 value{};
    OwnedEnvelope() {
        value.metadata.struct_size = sizeof(pf_report_metadata_v1);
        value.metadata.version = PF_REPORT_ENVELOPE_VERSION;
    }
    ~OwnedEnvelope() { BacktestEngine::free_report(&value.report); }
};

template<class Engine>
Work read_both(const Engine& engine) {
    const auto expected = ordered_closed_fold_oracle(engine);
    const auto& ledger = engine.rows();
    metrics::TradeStatsAccumulator seed_all(metrics::TradeFilter::ALL, initial_capital);
    metrics::TradeStatsAccumulator seed_longs(metrics::TradeFilter::LONG, initial_capital);
    metrics::TradeStatsAccumulator seed_shorts(metrics::TradeFilter::SHORT, initial_capital);
    TradeLedger::ReportSeed seed{};
    if (ledger.base() != 0) {
        // Supply the same unfinalized prefix contract as the sink consumer.
        // Seed construction is outside observation and is not an oracle.
        for (std::size_t index = 0; index < ledger.base(); ++index) {
            const auto row = engine.closed_trade(index);
            seed.net += row.pnl;
            const auto record = public_report_row(row);
            seed_all.fold(record);
            seed_longs.fold(record);
            seed_shorts.fold(record);
        }
        seed.all = &seed_all;
        seed.longs = &seed_longs;
        seed.shorts = &seed_shorts;
    }
    metrics::TradeStatsAccumulator all(metrics::TradeFilter::ALL, initial_capital);
    metrics::TradeStatsAccumulator longs(metrics::TradeFilter::LONG, initial_capital);
    metrics::TradeStatsAccumulator shorts(metrics::TradeFilter::SHORT, initial_capital);
    std::array<double, 3> percentages{};
    TradeLedger::ReportFold report;
    Work work{};
    {
        CountScope scope(work, ledger);
        percentages = engine.percentages();
        // fill_report_envelope_v1 refuses a running host. Use the existing
        // ledger seam for the ordinary report's net and all three filters.
        report = ledger.report_fold(initial_capital, seed, &all, &longs, &shorts);
    }
    equal_percent(percentages, expected.percentages);
    require(report.nonfinite_field == nullptr,
            "value mismatch: finite rows produced a deferred report error");
    require(report.total_trades == expected.count, "value mismatch: replay report count");
    equal_double(report.net, expected.net, "replay report net");
    equal_stats(all.finish(), expected.all);
    equal_stats(longs.finish(), expected.longs);
    equal_stats(shorts.finish(), expected.shorts);
    return work;
}

class AmendmentHost final : public source::PineStrategyHost {
public:
    AmendmentHost() {
        source::PineStrategyConfig config;
        config.initial_capital = closed_trade_query_work::initial_capital;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.125);
        set_syminfo_pointvalue(1.0);
    }

    void on_source_bar(const Bar&) override {
        const int trade = bar_index_ / 4;
        const int phase = bar_index_ % 4;
        if (bar_index_ == 0) {
            prefixes_checked = 0;
            run_work = {};
            previous_percent_rows_ = observed_percent_fold_rows(trades_);
            previous_report_rows_ = observed_report_fold_rows(trades_);
        }
        if (phase == 0) {
            strategy_entry("position", long_side(trade),
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN(), quantity(trade));
        } else if (phase == 1) {
            strategy_close("position");
        } else if (phase == 2) {
            require(closed_trade_count() == static_cast<std::size_t>(trade + 1),
                    "setup failure: close was not published before its read");
            check_closed_row(closed_trade(static_cast<std::size_t>(trade)), trade);
            OwnedEnvelope envelope;
            require(fill_report_envelope_v1(&envelope.value) == PF_REPORT_MODE_BUSY,
                    "setup failure: mid-run report gate changed");
            (void)read_both(*this);
            const auto percent_rows = observed_percent_fold_rows(trades_);
            const auto report_rows = observed_report_fold_rows(trades_);
            // The interval includes eager publication since the last close.
            // Start after run reset, whose fresh State resets the observers.
            run_work.percent_fold_rows += percent_rows - previous_percent_rows_;
            run_work.report_fold_rows += report_rows - previous_report_rows_;
            previous_percent_rows_ = percent_rows;
            previous_report_rows_ = report_rows;
            ++prefixes_checked;
        }
    }

    std::array<double, 3> percentages() const {
        return {avg_trade_percent(), avg_winning_trade_percent(), avg_losing_trade_percent()};
    }
    TradeLedger& rows() { return trades_; }
    const TradeLedger& rows() const { return trades_; }
    void seal_rows(std::size_t end) {
        for (auto index = trades_.base(); index < end; ++index)
            fold_trade_percent_prefix(trades_.read(index));
        trades_.seal(end);
    }
    int prefixes_checked = 0;
    Work run_work{};
private:
    std::uint64_t previous_percent_rows_ = 0, previous_report_rows_ = 0;
};

void exact_replay(const Work& work, std::size_t count, const char* context) {
    require(work.percent_fold_rows == count && work.report_fold_rows == count, context);
    require(overlay_percent_visits(work) == 0 && work.net_rows == 0 && work.metric_rows == 0,
            "replay escaped the ledger fold observers");
}

Work run_population(AmendmentHost& host, int count) {
    const auto bars = tape(count);
    host.run(bars.data(), static_cast<int>(bars.size()));
    check_completed(host);
    require(host.prefixes_checked == count,
            "setup failure: both folds were not read after every close");
    public_rows(host, count);
    exact_replay(read_both(host), 0, "quiet prefix read consumed rows");
    return host.run_work;
}

std::uint64_t consumed(const Work& work) {
    return work.percent_fold_rows + work.report_fold_rows;
}

void check_linear_work(const Work& work, int count) {
    require(work.percent_fold_rows >= static_cast<std::uint64_t>(count)
                && work.report_fold_rows >= static_cast<std::uint64_t>(count),
            "fold observer missed the real closing rows");
    require(consumed(work) <= work_per_close * static_cast<std::uint64_t>(count),
            "recent host amendments replayed full closed history");
}

void amend(TradeLedger& rows, std::size_t index, double scale) {
    rows.modify(index, [scale](Trade& row) {
        // Non-dyadic percentages expose changed addition order. Expectations
        // are recomputed from these actual public inputs, never cached totals.
        row.pnl_pct = row.pnl_pct * scale + 0.1;
        row.pnl *= scale;
        row.commission += 0.125;
        ++row.exit_bar_index;
    });
}

void recent_amendments() {
    AmendmentHost host;
    run_population(host, large_trades);
    const auto end = host.rows().size();
    require(end > replay_rows + 1, "setup failure: ring boundary is not exercised");
    amend(host.rows(), end - replay_rows, 2.0);
    const auto hash = host.broker_state_hash();
    exact_replay(read_both(host), replay_rows, "oldest in-window row was not replayed exactly");
    exact_replay(read_both(host), 0, "quiet amended read consumed rows");
    require(host.broker_state_hash() == hash, "derived replay changed the broker hash");

    amend(host.rows(), end - replay_rows - 1, 0.5);
    exact_replay(read_both(host), end, "out-of-window edit did not rebuild from the seed");

    amend(host.rows(), end - 3, 2.0);
    amend(host.rows(), end - 1, 0.5);
    exact_replay(read_both(host), 3, "two recent edits did not coalesce at their lowest index");
    amend(host.rows(), end - 1, 2.0);
    amend(host.rows(), end - 3, 0.5);
    exact_replay(read_both(host), 3, "later edit restored a snapshot newer than the first edit");

    // Repair, then amend another row whose pre-row state depended on the
    // previous edit. An index-only stale snapshot must not be reused.
    amend(host.rows(), end - 2, 2.0);
    exact_replay(read_both(host), 2, "snapshot generation aliased an earlier row version");
    amend(host.rows(), end - 2, 0.5);
    exact_replay(read_both(host), 2, "same-index amendment reused a stale pre-row state");
}

void permutation_and_rewind() {
    AmendmentHost host;
    run_population(host, large_trades);
    const auto end = host.rows().size();
    // Hand arithmetic: [2^54,-2^54,1] left-folds to 1 from zero, whereas
    // chunking the last two terms loses that 1. Public-row oracles also keep
    // the existing prefix, sign filters, streaks and extrema in their order.
    const std::array<double, 3> values{{0x1p54, -0x1p54, 1.0}};
    for (std::size_t index = 0; index < values.size(); ++index) {
        auto row = host.closed_trade(end - 3 + index);
        row.pnl = row.pnl_pct = values[index];
        host.rows().replace(end - 3 + index, row);
    }
    exact_replay(read_both(host), 3, "cancellation rows did not replay from their pre-row seed");
    const std::array<Trade, 3> saved{{host.closed_trade(end - 3), host.closed_trade(end - 2),
                                      host.closed_trade(end - 1)}};
    for (std::size_t index = 0; index < saved.size(); ++index)
        host.rows().replace(end - 3 + index, saved[(index + 1) % saved.size()]);
    exact_replay(read_both(host), 3, "tail permutation did not replay the ordered suffix");

    const auto first = host.closed_trade(end - 2);
    auto last = host.closed_trade(end - 1);
    host.rows().resize(end - 2);
    last.pnl *= 0.5;
    last.pnl_pct += 0.1;
    host.rows().push_back(last);
    host.rows().push_back(first);
    (void)read_both(host);
    exact_replay(read_both(host), 0, "quiet reappended report consumed rows");
}

void seal_then_amend() {
    AmendmentHost host;
    run_population(host, small_trades);
    const auto end = host.rows().size();
    // Even an unchanged publication boundary invalidates the old ring.
    host.rows().seal(0);
    amend(host.rows(), end - 1, 2.0);
    exact_replay(read_both(host), end, "seal retained an amendment snapshot");
    host.seal_rows(end - 8);
    exact_replay(read_both(host), 8, "sealed seeds did not invalidate both rings");
    amend(host.rows(), end - 8, 0.5);
    exact_replay(read_both(host), 8, "amended suffix did not resume the exact sealed seeds");
    host.seal_rows(end - 7);
    exact_replay(read_both(host), 7, "reseal retained a previous seed snapshot");
    amend(host.rows(), end - 1, 2.0);
    exact_replay(read_both(host), 1, "post-reseal amendment did not replay its row");
}

struct Failure {
    RunFailureValue value{};
    std::string message;
    int total = 0;
    double net = 0.0;
};

Failure full_failure(const BacktestEngine& engine) {
    ReportC report{};
    try {
        engine.fill_report(&report);
    } catch (const std::exception& error) {
        const Failure result{classify_run_failure(error), error.what(),
                             report.total_trades, report.net_profit};
        BacktestEngine::free_report(&report);
        return result;
    }
    BacktestEngine::free_report(&report);
    throw std::runtime_error("invalid public report unexpectedly succeeded");
}

Work read_failure(const AmendmentHost& host, std::size_t first_bad) {
    // FULL keeps its independent numeric/error producer. It supplies the
    // typed error and partial outputs; the cached seam must preserve both.
    const auto expected = full_failure(host);
    metrics::TradeStatsAccumulator all(metrics::TradeFilter::ALL, initial_capital);
    TradeLedger::ReportFold fold;
    Work work{};
    {
        CountScope scope(work, host.rows());
        fold = host.rows().report_fold(initial_capital, {}, &all);
    }
    require(fold.nonfinite_field && fold.failure_row == first_bad
                && fold.total_trades == static_cast<std::size_t>(expected.total),
            "replay changed the first invalid row or partial count");
    equal_double(fold.net, expected.net, "replay partial net before the first numeric failure");
    require(expected.value.args
                && expected.value.args->at("field") == fold.nonfinite_field,
            "replay changed the first invalid field");
    try {
        (void)all.finish();
    } catch (const std::exception& error) {
        const auto actual = classify_run_failure(error);
        require(actual.code == expected.value.code && actual.args && expected.value.args
                    && *actual.args == *expected.value.args && error.what() == expected.message,
                "replay changed the typed numeric error");
        return work;
    }
    throw std::runtime_error("replayed invalid accumulator unexpectedly finished");
}

void invalid_recent_rows() {
    AmendmentHost host;
    run_population(host, small_trades);
    const auto end = host.rows().size();
    const auto earlier = host.closed_trade(end - 3);
    const auto later = host.closed_trade(end - 1);
    host.rows().modify(end - 3, [](Trade& row) {
        row.commission = std::numeric_limits<double>::infinity();
    });
    host.rows().modify(end - 1, [](Trade& row) {
        row.entry_price = std::numeric_limits<double>::quiet_NaN();
    });
    require(read_failure(host, end - 3).report_fold_rows == 1,
            "invalid suffix replay read beyond its first failure");
    require(read_failure(host, end - 3).report_fold_rows == 0,
            "quiet invalid report replayed its failure");
    host.rows().replace(end - 3, earlier);
    require(read_failure(host, end - 1).report_fold_rows == 3,
            "repair of the first failure did not reach the next failing row in order");
    host.rows().replace(end - 1, later);
    const auto recovered = read_both(host);
    require(recovered.percent_fold_rows == 3 && recovered.report_fold_rows == 1,
            "failure repair did not preserve each fold's independent cursor");
    exact_replay(read_both(host), 0, "quiet recovered report consumed rows");
}

class EngineCopy final : public BacktestEngine {
public:
    explicit EngineCopy(const BacktestEngine& engine) : BacktestEngine(engine) {}
    void on_bar(const Bar&) override {
        throw std::logic_error("setup failure: query copy received bar dispatch");
    }
    std::array<double, 3> percentages() const {
        return {avg_trade_percent(), avg_winning_trade_percent(), avg_losing_trade_percent()};
    }
    TradeLedger& rows() { return trades_; }
    const TradeLedger& rows() const { return trades_; }
};

void copies_and_reset() {
    AmendmentHost host;
    run_population(host, small_trades);
    const auto end = host.rows().size();
    EngineCopy copy(host);
    exact_replay(read_both(copy), end, "engine copy retained a warm fold or ring");
    amend(host.rows(), end - 3, 2.0);
    amend(copy.rows(), end - 2, 0.5);
    exact_replay(read_both(copy), 2, "copy amendment did not use its own snapshot");
    exact_replay(read_both(host), 3, "original amendment did not use its own snapshot");
    exact_replay(read_both(copy), 0, "original amendment invalidated the copy");
    copy.rows() = host.rows();
    amend(copy.rows(), end - 1, 2.0);
    exact_replay(read_both(copy), end, "copy assignment retained a previous ring");

    TradeLedger moved(host.rows());
    (void)moved.percent_fold({});
    (void)moved.report_fold(initial_capital, {});
    copy.rows() = std::move(moved);
    amend(copy.rows(), end - 1, 0.5);
    exact_replay(read_both(copy), end, "move assignment retained a transferred ring");

    std::vector<Trade> saved;
    for (std::size_t index = 0; index < end; ++index)
        saved.push_back(host.closed_trade(index));
    host.rows().clear();
    for (auto row : saved) {
        row.pnl_pct *= 0.5;
        host.rows().push_back(std::move(row));
    }
    amend(host.rows(), end - 1, 2.0);
    exact_replay(read_both(host), end, "clear retained the previous row generation");
    const auto reset = run_population(host, small_trades);
    check_linear_work(reset, small_trades);
    host.run(nullptr, 0);
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "setup failure: empty run reset failed");
    exact_replay(read_both(host), 0, "empty run retained the previous folds");
}

} // namespace

int main() {
    try {
        AmendmentHost small_host, large_host;
        const auto small = run_population(small_host, small_trades);
        const auto large = run_population(large_host, large_trades);
        check_linear_work(small, small_trades);
        check_linear_work(large, large_trades);
        require(consumed(large) <= 4 * consumed(small) + 2 * replay_rows,
                "fourfold close population caused superlinear fold work");
        recent_amendments();
        permutation_and_rewind();
        seal_then_amend();
        invalid_recent_rows();
        copies_and_reset();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_closed_trade_amendment_replay: %s\n", error.what());
        return 1;
    }
}
