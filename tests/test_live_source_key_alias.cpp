// Public run/C ABI outputs, with the production adapter optionally
// compiled with a narrow key. No copied collision-verification implementation.
#include <pineforge/native_module.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

class SourceKeyAliasProbe : public source::PineStrategyHost {
public:
    std::string first = "A";
    std::string second = "E";
    bool use_order = false;

    SourceKeyAliasProbe() {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 1.0;
        config.pyramiding = 10;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(1.0);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 1) {
            const auto& id = bar_index_ == 0 ? first : second;
            const double limit = bar_index_ == 0 ? 90.0 : 80.0;
            if (use_order) strategy_order(id, true, 1.0, limit);
            else strategy_entry(id, true, limit, na, 1.0);
        }
        if (bar_index_ == 3) strategy_close_all();
    }
};
} // namespace

PINEFORGE_EXPORT_NATIVE_STRATEGY(SourceKeyAliasProbe);

namespace {
void run_case(const std::string& first, const std::string& second, bool use_order,
              bool refuse, int expected_trades) {
    const auto strategy = strategy_create(nullptr);
    CHECK(strategy != nullptr);
    if (!strategy) return;
    auto* probe = dynamic_cast<SourceKeyAliasProbe*>(static_cast<BacktestEngine*>(strategy));
    CHECK(probe != nullptr);
    if (!probe) { strategy_free(strategy); return; }
    probe->first = first;
    probe->second = second;
    probe->use_order = use_order;
    std::vector<pf_bar_t> bars{
        {100.0, 100.0, 100.0, 100.0, 1.0, 1700000040000LL},
        {100.0, 100.0, 100.0, 100.0, 1.0, 1700000100000LL},
        {90.0, 90.0, 80.0, 80.0, 1.0, 1700000160000LL},
        {80.0, 80.0, 80.0, 80.0, 1.0, 1700000220000LL},
        {80.0, 80.0, 80.0, 80.0, 1.0, 1700000280000LL},
    };
    pf_report_t report{};
    run_backtest_full(strategy, bars.data(), static_cast<int>(bars.size()),
                      "1", "1", 0, 4, PF_MAGNIFIER_ENDPOINTS, &report);
    const std::string code = strategy_get_last_error_code(strategy);
    const std::string args = strategy_get_last_error_args(strategy);
    const std::string error = strategy_get_last_error(strategy);
    const int status = strategy_last_run_status(strategy);
    if (refuse) {
        if (code != "engine_invariant" || status != 1) {
            std::fprintf(stderr,
                "expected source-key collision refusal for %s/%s; got status=%d code=%s trades=%d error=%s\n",
                first.c_str(), second.c_str(), status, code.c_str(),
                report.trades_len, error.c_str());
        }
        CHECK(code == "engine_invariant");
        CHECK(args == "{}");
        CHECK(status == 1);
        CHECK(error.find("Pine live source key collision") != std::string::npos);
        CHECK(report.trades_len == 0);
    } else {
        CHECK(code.empty());
        CHECK(error.empty());
        CHECK(status == 0);
        CHECK(report.trades_len == expected_trades);
        // Hand oracle: the first live buy fills at 90, the second at 80;
        // both close at 80. Replacing the same id removes the 90 order.
        bool found_first = false, found_second = false;
        for (int i = 0; i < report.trades_len; ++i) {
            const auto& trade = report.trades[i];
            const char* entry_id = strategy_closed_trade_entry_id(strategy, i);
            CHECK(entry_id != nullptr);
            if (!entry_id) continue;
            CHECK(trade.qty == 1.0);
            CHECK(trade.exit_price == 80.0);
            if (first != second && std::string(entry_id) == first) {
                found_first = true;
                CHECK(trade.entry_price == 90.0);
                CHECK(trade.pnl == -10.0);
            } else if (std::string(entry_id) == second) {
                found_second = true;
                CHECK(trade.entry_price == 80.0);
                CHECK(trade.pnl == 0.0);
            } else {
                CHECK(false);
            }
        }
        CHECK(found_second);
        CHECK(found_first == (first != second));
    }
    report_free(&report);
    strategy_free(strategy);
}

class SourceKeyLookupProbe final : public source::PineStrategyHost {
public:
    SourceKeyLookupProbe(const std::string& first, const std::string& second)
        : first_(first), second_(second) {
        source::PineStrategyConfig config;
        config.initial_capital = 100.0;
        config.default_qty_value = 1.0;
        config.pyramiding = 10;
        config.margin_long = config.margin_short = 50.0;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(1.0);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry(first_, true, na, 110.0, 1.0);
        if (bar_index_ == 1) strategy_entry(second_, true, na, 110.0, 3.0);
        if (bar_index_ == 3) strategy_close_all();
    }

private:
    std::string first_, second_;
};

void run_lookup_case(const std::string& first, const std::string& second,
                     bool refuse, int expected_trades) {
    SourceKeyLookupProbe probe(first, second);
    std::vector<Bar> bars{
        {100.0, 100.0, 100.0, 100.0, 1.0, 1700000040000LL},
        {100.0, 100.0, 100.0, 100.0, 1.0, 1700000100000LL},
        {100.0, 110.0, 100.0, 110.0, 1.0, 1700000160000LL},
        {110.0, 110.0, 110.0, 110.0, 1.0, 1700000220000LL},
        {110.0, 110.0, 110.0, 110.0, 1.0, 1700000280000LL},
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));
    const auto strategy = static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(&probe));
    const std::string code = strategy_get_last_error_code(strategy);
    const std::string args = strategy_get_last_error_args(strategy);
    const int status = strategy_last_run_status(strategy);
    if (refuse) {
        if (code != "engine_invariant" || status != 1) {
            std::fprintf(stderr,
                "expected source-key lookup collision refusal for %s/%s; got status=%d code=%s trades=%d error=%s\n",
                first.c_str(), second.c_str(), status, code.c_str(),
                probe.trade_count(), probe.last_error().c_str());
        }
        CHECK(code == "engine_invariant");
        CHECK(args == "{}");
        CHECK(status == 1);
        CHECK(probe.last_error().find("Pine live source key collision") != std::string::npos);
        CHECK(probe.trade_count() == 0);
    } else {
        CHECK(code.empty());
        CHECK(probe.last_error().empty());
        CHECK(status == 0);
        CHECK(probe.trade_count() == expected_trades);
        // Hand oracle: at the 100 signal the first stop needs 1*100*50% = 50
        // of margin, within the 100 equity. The second needs 3*100*50% = 150,
        // so admission returns before submit_or_replace. A same-id refusal
        // cancels the first stop; a distinct id must leave it alone. That
        // surviving buy fills at 110 on bar 2 and closes at 110 on bar 4:
        // (110-110)*1 = 0. Guarding only submit_or_replace cannot pass this.
        for (int i = 0; i < probe.trade_count(); ++i) {
            const auto trade = probe.get_trade(i);
            CHECK(trade.entry_id == first);
            CHECK(trade.entry_bar_index == 2);
            CHECK(trade.exit_bar_index == 4);
            CHECK(trade.entry_price == 110.0);
            CHECK(trade.exit_price == 110.0);
            CHECK(trade.qty == 1.0);
            CHECK(trade.pnl == 0.0);
        }
    }
}
} // namespace

int main() {
    // FNV basis mod 4 = 3; prime mod 4 = 3. A=65 and E=69 both
    // equal 1 mod 4: ((3 xor 1)*3*3) mod 4 = 2 after the NUL
    // separator. B=66 gives ((3 xor 2)*3*3) mod 4 = 1.
    // The separator belongs to source_key, never to an input id.
#ifdef PINEFORGE_TEST_NARROW_SOURCE_KEY
    constexpr bool narrow = true;
#else
    constexpr bool narrow = false;
#endif
    for (const bool use_order : {false, true}) {
        run_case("A", "E", use_order, narrow, 2);
        run_case("E", "A", use_order, narrow, 2);
        run_case("A", "A", use_order, false, 1);
        run_case("A", "B", use_order, false, 2);
        // Arbitrary-length ids are compared in their entirety.
        const std::string prefix(512, 'Q');
        run_case(prefix + "A", prefix + "E", use_order, narrow, 2);
    }
    // Admission's cancellation lookup returns without ever submitting the
    // unaffordable second stop, so it must verify identity independently.
    run_lookup_case("A", "E", narrow, 1);
    run_lookup_case("E", "A", narrow, 1);
    run_lookup_case("A", "A", false, 0);
    run_lookup_case("A", "B", false, 1);
    return failures == 0 ? 0 : 1;
}
