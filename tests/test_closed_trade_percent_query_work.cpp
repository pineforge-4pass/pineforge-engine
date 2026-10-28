// Values precede the work verdict. The original accessor bodies are compiled
// from the test-only header overlay; mutation controls start with real runs.
#include "closed_trade_query_work.hpp"
#include <pineforge/source/pine_strategy_host.hpp>
#include <utility>

#ifndef PINEFORGE_TEST_CLOSED_TRADE_PERCENT_OVERLAY
#error "setup failure: percentage loop counting overlay is missing"
#endif

namespace {
using namespace closed_trade_query_work;

class PercentHost final : public source::PineStrategyHost {
public:
    explicit PercentHost(int target) : target_(target) {
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
        if (phase == 0) {
            strategy_entry("position", long_side(trade),
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN(), quantity(trade));
        } else if (phase == 1) {
            strategy_close("position");
        } else if (phase == 2) {
            // Read every newly published prefix, so caching only a previously
            // queried prefix cannot hide a full rebuild on the first read.
            const auto closed = trades_.size();
            if (closed < rows_seen) rows_seen = 0;
            const auto newly_closed = closed - rows_seen;
            const auto expected = percent_oracle(*this, trade + 1);
            check_counts(expected);
            std::array<double, 3> actual{};
            const auto before_work = percent_visits(after_close);
            {
                CountScope scope(after_close, trades_);
                actual = read();
            }
            equal_percent(actual, expected);
            // Keep the verdict outside run(): a work failure must not be
            // mistaken for a setup failure by check_completed().
            incremental_work_matches = incremental_work_matches && newly_closed != 0
                && percent_visits(after_close) - before_work == newly_closed;
            rows_seen = closed;
            ++prefixes_checked;
        } else if (trade + 1 == target_) {
            const auto expected = percent_oracle(*this, target_);
            check_counts(expected);
            for (std::size_t query = 0; query < query_count; ++query) {
                {
                    CountScope scope(quiet[query], trades_);
                    final_values = read();
                }
                equal_percent(final_values, expected);
            }
            quiet_checked = true;
        }
    }

    Work after_close{};
    std::array<Work, query_count> quiet{};
    std::array<double, 3> final_values{};
    std::size_t rows_seen = 0;
    int prefixes_checked = 0;
    bool quiet_checked = false;
    bool incremental_work_matches = true;

    std::array<double, 3> percentages() const { return read(); }
    TradeLedger& rows() { return trades_; }
    const TradeLedger& rows() const { return trades_; }

    void seal_rows(std::size_t end) {
        for (std::size_t index = trades_.base(); index < end; ++index)
            fold_trade_percent_prefix(trades_.read(index));
        trades_.seal(end);
    }

private:
    std::array<double, 3> read() const {
        return {avg_trade_percent(), avg_winning_trade_percent(), avg_losing_trade_percent()};
    }

    void check_counts(const PercentOracle& expected) const {
        require(count_wintrades() == static_cast<int>(expected.counts[1])
                    && count_losstrades() == static_cast<int>(expected.counts[2])
                    && eventrades() == static_cast<int>(expected.evens),
                "value mismatch: strategy count accessors");
    }

    int target_;
};

void forwarding_control(const PercentHost& host) {
    std::vector<Trade> rows;
    for (std::size_t index = 0; index < host.closed_trade_count(); ++index)
        rows.push_back(host.closed_trade(index));
    Work observed{};
    PercentOracle forwarded;
    std::size_t index = 0;
    {
        CountScope scope(observed, host.rows());
        // This forwarding loop is emitted into the same header copy and
        // passes through the same body-insertion step as the real accessors.
        // A missing overlay cannot satisfy the control by calling a counter.
        percent_overlay_control(rows, [&](const Trade& row) {
            require(&row == &rows[index++],
                    "setup failure: percentage overlay did not forward the original row");
            forwarded.add(row);
        });
    }
    require(observed.percent_control_rows == host.closed_trade_count()
                && observed.percent_control_rows != 0,
            "setup failure: percentage forwarding control observed no visits");
    std::printf("CONTROL VERIFIED closed=%zu percentage-body-visits=%llu\n",
                host.closed_trade_count(),
                static_cast<unsigned long long>(observed.percent_control_rows));
    // The observer must forward every row without changing its numeric bits.
    equal_percent(forwarded.values(),
                  percent_oracle(host, static_cast<int>(host.closed_trade_count())));
}

struct Sample {
    Work after_close{};
    std::array<Work, query_count> quiet{};
};

void check_percentage_work(const PercentHost& host) {
    require(host.incremental_work_matches,
            "percentage first read did not consume exactly the newly closed rows");
    for (const auto& quiet : host.quiet)
        require(percent_visits(quiet) == 0,
                "quiet percentage read consumed cached rows");
}

Sample sample(int trades) {
    PercentHost host(trades);
    const auto bars = tape(trades);
    host.run(bars.data(), static_cast<int>(bars.size()));
    check_completed(host);
    const auto rows = public_rows(host, trades);
    require(rows.size() == static_cast<std::size_t>(trades)
                && host.prefixes_checked == trades && host.quiet_checked,
            "setup failure: percentage reads did not cover every close and the quiet evaluation");
    forwarding_control(host);
    const auto expected = percent_oracle(host, trades);
    equal_percent(host.final_values, expected);
    std::printf("VALUES VERIFIED closed=%d wins=%zu losses=%zu evens=%zu "
                "percent=%a/%a/%a first-read-visits=%llu\n",
                trades, expected.counts[1], expected.counts[2], expected.evens,
                host.final_values[0], host.final_values[1], host.final_values[2],
                static_cast<unsigned long long>(percent_visits(host.after_close)));
    check_percentage_work(host);
    require(percent_visits(host.after_close) == static_cast<std::uint64_t>(trades),
            "percentage first reads did not consume exactly the newly closed rows");
    for (std::size_t query = 0; query < query_count; ++query)
        std::printf("quiet closed=%d query=%zu row-visits=%llu/%llu/%llu fold-rows=%llu total=%llu\n",
                    trades, query,
                    static_cast<unsigned long long>(host.quiet[query].percent_rows[0]),
                    static_cast<unsigned long long>(host.quiet[query].percent_rows[1]),
                    static_cast<unsigned long long>(host.quiet[query].percent_rows[2]),
                    static_cast<unsigned long long>(host.quiet[query].percent_fold_rows),
                    static_cast<unsigned long long>(percent_visits(host.quiet[query])));
    return {host.after_close, host.quiet};
}

template<class Engine>
Work current_percentages(const Engine& engine, const char* context) {
    Work observed{};
    try {
        CountScope scope(observed, engine.rows());
        equal_percent(engine.percentages(), percent_oracle(engine));
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(context) + ": " + error.what());
    }
    return observed;
}

void run_population(PercentHost& host, int count) {
    const auto bars = tape(count);
    host.run(bars.data(), static_cast<int>(bars.size()));
    check_completed(host);
    public_rows(host, count);
    current_percentages(host, "real run");
    check_percentage_work(host);
}

void old_row_amendments() {
    PercentHost host(large_trades);
    run_population(host, large_trades);
    require(host.rows().resident_base() > 1,
            "setup failure: amended rows must precede the descriptor window");
    const auto resident_rows = host.rows().size() - host.rows().base();
    // These are edits through the same public ledger interface used by the
    // source host. No private storage or precomputed aggregate is supplied.
    // Multiplying a published percentage changes the oracle's input; its
    // expected average still comes only from the ordered public row fold.
    host.rows().modify(0, [](Trade& row) { row.pnl_pct *= 2.0; });
    const auto amended = current_percentages(host, "old winning row modified after read");
    // A covered-row amendment invalidates the fold; the rebuild consumes
    // exactly the current resident suffix [base(), size()).
    require(percent_visits(amended) == resident_rows,
            "covered-row amendment did not rebuild exactly resident rows");
    Trade loss = host.closed_trade(1);
    loss.pnl_pct *= 0.5;
    host.rows().replace(1, loss);
    const auto before = host.broker_state_hash();
    const auto replaced = current_percentages(host, "old losing row replaced after read");
    require(percent_visits(replaced) == resident_rows,
            "covered-row replacement did not rebuild exactly resident rows");
    const auto quiet = current_percentages(host, "repeated reads after amendments");
    require(percent_visits(quiet) == 0,
            "repeated percentage read after amendment consumed cached rows");
    require(host.broker_state_hash() == before,
            "percentage query changed the broker state hash");
}

void truncate_and_reappend() {
    PercentHost host(large_trades);
    run_population(host, large_trades);
    const auto end = host.closed_trade_count();
    const auto resident_rows = host.rows().size() - host.rows().base();
    std::vector<Trade> tail;
    for (std::size_t index = end - 3; index < end; ++index)
        tail.push_back(host.closed_trade(index));
    host.rows().resize(end - tail.size());
    // Re-publish a different tail at the SAME end before another read.
    // An end-only key, or a missing rewind generation, would alias it.
    for (auto row : tail) {
        row.pnl_pct *= 2.0;
        host.rows().push_back(std::move(row));
    }
    const auto reappended = current_percentages(host, "truncate and reappend at unchanged row count");
    require(percent_visits(reappended) == resident_rows,
            "truncate and reappend did not rebuild exactly resident rows");
    host.rows().resize(end - tail.size());
    const auto truncated_rows = host.rows().size() - host.rows().base();
    const auto truncated = current_percentages(host, "read truncated history");
    require(percent_visits(truncated) == truncated_rows,
            "truncate rebuild did not consume exactly resident rows");
    const auto append_begin = host.rows().size();
    for (const auto& row : tail) host.rows().push_back(row);
    const auto appended_rows = host.rows().size() - append_begin;
    const auto appended = current_percentages(host, "unread appended tail");
    require(percent_visits(appended) == appended_rows,
            "append read did not consume exactly the newly appended rows");
    host.rows().resize(end - tail.size());
    host.rows().resize(end);
    const auto expanded_rows = host.rows().size() - host.rows().base();
    const auto expanded = current_percentages(host, "resize with default rows");
    require(percent_visits(expanded) == expanded_rows,
            "resize rebuild did not consume exactly resident rows");
    for (std::size_t index = 0; index < tail.size(); ++index)
        host.rows().replace(end - tail.size() + index, tail[index]);
    const auto restored = current_percentages(host, "replace resized rows");
    require(percent_visits(restored) == expanded_rows,
            "restored rows did not rebuild exactly resident rows");
}

void reset_and_reuse() {
    PercentHost host(small_trades);
    run_population(host, small_trades);
    host.rows().modify(0, [](Trade& row) { row.pnl_pct *= 2.0; });
    current_percentages(host, "warm amended run before reuse");
    run_population(host, small_trades);
    host.run(nullptr, 0);
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "setup failure: empty reuse run failed");
    current_percentages(host, "empty reset after populated run");
    run_population(host, small_trades);

    std::vector<Trade> saved;
    for (std::size_t index = 0; index < host.closed_trade_count(); ++index)
        saved.push_back(host.closed_trade(index));
    // clear and append to the old size without a query in between.
    host.rows().clear();
    for (auto row : saved) {
        row.pnl_pct *= 0.5;
        host.rows().push_back(std::move(row));
    }
    current_percentages(host, "clear and repopulate at unchanged row count");
}

// The public strategy hosts deliberately forbid copying. Copy their engine
// base, as the existing engine-copy tests do, and exercise its real ledger
// and aggregate accessors. Dispatch on this query-only copy is an error.
class PercentEngineCopy final : public BacktestEngine {
public:
    explicit PercentEngineCopy(const BacktestEngine& engine) : BacktestEngine(engine) {}
    void on_bar(const Bar&) override {
        throw std::logic_error("setup failure: query copy received bar dispatch");
    }
    std::array<double, 3> percentages() const {
        return {avg_trade_percent(), avg_winning_trade_percent(), avg_losing_trade_percent()};
    }
    TradeLedger& rows() { return trades_; }
    const TradeLedger& rows() const { return trades_; }
};

void copy_then_diverge() {
    PercentHost host(small_trades);
    run_population(host, small_trades);
    PercentEngineCopy copy(host);
    current_percentages(copy, "copied engine");
    const auto copied_before = percent_oracle(copy);
    host.rows().modify(0, [](Trade& row) { row.pnl_pct *= 2.0; });
    current_percentages(host, "original engine diverged");
    current_percentages(copy, "copy retained original rows");
    equal_percent(copy.percentages(), copied_before);
    const auto original_before = percent_oracle(host);
    copy.rows().modify(1, [](Trade& row) { row.pnl_pct *= 0.5; });
    current_percentages(copy, "copied engine diverged");
    current_percentages(host, "original retained its own rows");
    equal_percent(host.percentages(), original_before);

    // Assignment must not reuse the destination's warm fold just because
    // the source has the same size, base and local mutation count.
    copy.rows() = host.rows();
    current_percentages(copy, "ledger copy assignment at unchanged row count");
    TradeLedger moved(host.rows());
    copy.rows().modify(1, [](Trade& row) { row.pnl_pct *= 2.0; });
    current_percentages(copy, "warm destination before ledger move");
    copy.rows() = std::move(moved);
    current_percentages(copy, "ledger move assignment at unchanged row count");
}

void seal_and_amend() {
    PercentHost host(large_trades);
    run_population(host, large_trades);
    host.seal_rows(large_trades / 2);
    current_percentages(host, "sealed prefix after warm read");
    const auto first = host.rows().base();
    host.rows().modify(first, [](Trade& row) { row.pnl_pct *= 2.0; });
    current_percentages(host, "amended row after sealed prefix");
    host.seal_rows(first + 3);
    current_percentages(host, "resealed amended prefix");
    run_population(host, large_trades);
}

} // namespace

int main() {
    try {
        const auto small = sample(small_trades);
        const auto large = sample(large_trades);
        old_row_amendments();
        truncate_and_reappend();
        reset_and_reuse();
        copy_then_diverge();
        seal_and_amend();
        bool replayed = overlay_percent_visits(small.after_close) != 0
                     || overlay_percent_visits(large.after_close) != 0;
        for (std::size_t query = 0; query < query_count; ++query) {
            for (std::size_t accessor = 0; accessor < 3; ++accessor) {
                const auto a = small.quiet[query].percent_rows[accessor];
                const auto b = large.quiet[query].percent_rows[accessor];
                replayed = replayed || a != 0 || b != 0 || b > a;
            }
            const auto small_work = percent_visits(small.quiet[query]);
            const auto large_work = percent_visits(large.quiet[query]);
            replayed = replayed || small_work != 0 || large_work != 0
                     || large_work > small_work;
        }
        require(!replayed,
                "closed-trade percentage queries replayed history; expected zero row visits");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_closed_trade_percent_query_work: %s\n", error.what());
        return 1;
    }
}
