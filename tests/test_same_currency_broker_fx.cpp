// #954: the five one-minute bars and the source strategy from the report.
// The compiled capability is the same for omitted currency and currency.NONE.
#include <pineforge/pineforge.h>
#include <pineforge/checked_settings.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace pineforge;

constexpr std::int64_t start = 1704067200000;
constexpr std::int64_t minute = 60000;
constexpr double rate = 1.25;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

enum class Fx { none, scalar, curve };

class ProfitHost final : public source::PineStrategyHost {
public:
    explicit ProfitHost(const char* declaration, bool unresolved = false)
        : currency(declaration), unresolved(unresolved) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 10000;
        configure_pine_strategy(cfg);
    }

    void on_source_bar(const Bar& bar) override {
        ++calls;
        const int index = pine_bar_index();
        if (index == 0) strategy_entry("L", true, missing, missing, 1.0);
        if (index == 2) strategy_close("L");
        if (index == 4) {
            account = currency == "NONE" ? syminfo_.currency : currency;
            observed_quote = syminfo_.currency;
            using Dir = CurrencyConversionDirection;
            one_in_account = strategy_convert_identity_or_warn(1, Dir::to_account, 0,
                                                                account, bar.timestamp);
            one_in_symbol = strategy_convert_identity_or_warn(1, Dir::to_symbol, 1,
                                                               account, bar.timestamp);
            booked = net_profit();
        }
    }

    const std::string currency;
    const bool unresolved;
    int calls = 0, copies = 0;
    std::string account, observed_quote;
    double one_in_account = missing, one_in_symbol = missing, booked = missing;
};

template<class Actual, class Expected>
bool precondition(const std::string& row, const char* field, const Actual& actual,
                  const Expected& expected) {
    if (actual == expected) return true;
    std::cerr << "NOT COMPARED " << row << ": " << field << " expected=" << expected
              << " actual=" << actual << '\n';
    return false;
}

bool check(ProfitHost& host, const std::string& row, const char* quote,
           const char* expected_account, double expected_profit, Fx fx) {
    if (!precondition(row, "set chart currency",
                      strategy_set_syminfo_string(&host, "currency", quote), 0)) return false;
    std::array<Bar, 5> bars{};
    constexpr double prices[] = {100, 100, 104, 104, 104};
    for (int i = 0; i < 5; ++i) {
        const double p = prices[i];
        bars[i] = {p, p, p, p, 1, start + i * minute};
    }
    host.calls = 0;
    host.copies = 0;
    host.account.clear();
    host.observed_quote.clear();
    host.one_in_account = host.one_in_symbol = host.booked = missing;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (!precondition(row, "run error", host.last_error(), std::string{}) ||
        !precondition(row, "run status", host.last_run_status(), 0) ||
        !precondition(row, "receipt copies", host.copies, fx == Fx::none ? 0 : 1) ||
        !precondition(row, "bars evaluated", host.calls, 5) ||
        !precondition(row, "chart currency", host.observed_quote, quote) ||
        !precondition(row, "account", host.account, expected_account) ||
        !precondition(row, "convert_to_account(1)", host.one_in_account, 1.0) ||
        !precondition(row, "convert_to_symbol(1)", host.one_in_symbol, 1.0) ||
        !precondition(row, "closed trades", host.trade_count(), 1)) return false;
    const auto trade = host.get_trade(0);
    if (!precondition(row, "long", trade.is_long, true) ||
        !precondition(row, "entry id", trade.entry_id, "L") ||
        !precondition(row, "entry time", trade.entry_time, bars[1].timestamp) ||
        !precondition(row, "exit time", trade.exit_time, bars[3].timestamp) ||
        !precondition(row, "entry price", trade.entry_price, 100.0) ||
        !precondition(row, "exit price", trade.exit_price, 104.0) ||
        !precondition(row, "quantity", trade.qty, 1.0) ||
        !precondition(row, "commission", trade.commission, 0.0) ||
        !precondition(row, "open at end", trade.open_at_end, false)) return false;
    if (host.booked != expected_profit || trade.pnl != expected_profit) {
        std::cerr << "FAIL " << row << ": expected=" << expected_profit
                  << " netprofit=" << host.booked << " trade.pnl=" << trade.pnl << '\n';
        return false;
    }
    return true;
}
} // namespace

// The existing per-module ABI. Each instance represents a script with an
// immutable declaration; the real emitter also emits NONE for an omitted one.
extern "C" uint32_t strategy_capabilities_api_version() { return PF_CAPABILITIES_API_VERSION; }
extern "C" int strategy_capabilities_receipt(pf_strategy_t s, char* json, size_t capacity,
                                             size_t* required, char* error, size_t error_size) {
    return pineforge::checked_settings::boundary(error, error_size, [&] {
        pineforge::checked_settings::require(s != nullptr, "missing strategy handle");
        auto& host = *static_cast<ProfitHost*>(static_cast<pineforge::BacktestEngine*>(s));
        const std::string declaration = host.unresolved ? "null" : "\"currency." + host.currency + "\"";
        pineforge::checked_settings::receipt(
            "{\"declarations\":{\"currency\":" + declaration +
            "},\"requests\":[],\"requirements\":{},\"unresolved\":[" +
            (host.unresolved ? "\"currency\"" : "") + "],\"version\":1}",
            json, capacity, required);
        ++host.copies;
    });
}

int main() {
    int compared = 0;
    for (const std::string label : {"omitted", "NONE", "USDT", "EUR"}) {
        for (const auto fx : {Fx::none, Fx::scalar, Fx::curve}) {
            ProfitHost host(label == "omitted" ? "NONE" : label.c_str());
            const std::string row = label + "/" +
                (fx == Fx::none ? "no-rate" : fx == Fx::scalar ? "scalar" : "curve");
            strategy_set_syminfo_pointvalue(&host, 1);
            if (fx == Fx::scalar &&
                !precondition(row, "set scalar", strategy_set_account_currency_fx_scalar(&host, rate), 0))
                continue;
            if (fx == Fx::curve &&
                !precondition(row, "set curve",
                              strategy_set_account_currency_fx_series(&host, &start, &rate, 1), 0))
                continue;
            const bool euro = label == "EUR";
            if (check(host, row, "USDT", euro ? "EUR" : "USDT",
                      euro && fx != Fx::none ? 5 : 4, fx)) ++compared;
            if (label == "USDT" && fx != Fx::none) {
                // Same handle and installed input, now on a different chart.
                if (check(host, row + "/reuse-EUR-chart", "EUR", "USDT", 5, fx)) ++compared;
            }
        }
    }
    ProfitHost unresolved("EUR", true);
    strategy_set_syminfo_pointvalue(&unresolved, 1);
    if (precondition("unresolved/scalar", "set scalar",
                     strategy_set_account_currency_fx_scalar(&unresolved, rate), 0) &&
        check(unresolved, "unresolved/scalar", "USDT", "EUR", 5, Fx::scalar)) ++compared;
    if (compared != 15) return 1;
    std::cout << "PASS #954: 15 profit comparisons\n";
    return 0;
}
