// Values precede the work verdict. The original accessor bodies are compiled
// from the test-only header overlay; no trade ledger is seeded or amended.
#include "closed_trade_query_work.hpp"
#include <pineforge/source/pine_strategy_host.hpp>

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
            const auto expected = percent_oracle(*this, trade + 1);
            check_counts(expected);
            std::array<double, 3> actual{};
            {
                CountScope scope(after_close);
                actual = read();
            }
            equal_percent(actual, expected);
            ++prefixes_checked;
        } else if (trade + 1 == target_) {
            const auto expected = percent_oracle(*this, target_);
            check_counts(expected);
            for (std::size_t query = 0; query < query_count; ++query) {
                {
                    CountScope scope(quiet[query]);
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
    int prefixes_checked = 0;
    bool quiet_checked = false;

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
        CountScope scope(observed);
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
    for (std::size_t query = 0; query < query_count; ++query)
        std::printf("quiet closed=%d query=%zu row-visits=%llu/%llu/%llu\n",
                    trades, query,
                    static_cast<unsigned long long>(host.quiet[query].percent_rows[0]),
                    static_cast<unsigned long long>(host.quiet[query].percent_rows[1]),
                    static_cast<unsigned long long>(host.quiet[query].percent_rows[2]));
    return {host.after_close, host.quiet};
}

} // namespace

int main() {
    try {
        const auto small = sample(small_trades);
        const auto large = sample(large_trades);
        bool replayed = percent_visits(small.after_close) != 0
                     || percent_visits(large.after_close) != 0;
        for (std::size_t query = 0; query < query_count; ++query) {
            for (std::size_t accessor = 0; accessor < 3; ++accessor) {
                const auto a = small.quiet[query].percent_rows[accessor];
                const auto b = large.quiet[query].percent_rows[accessor];
                replayed = replayed || a != 0 || b != 0 || b > a;
            }
        }
        require(!replayed,
                "closed-trade percentage queries replayed history; expected zero row visits");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_closed_trade_percent_query_work: %s\n", error.what());
        return 1;
    }
}
