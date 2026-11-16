// The script's account-currency conversions on the source host
// (PineStrategyHost::strategy_convert_to_account, strategy_convert_to_symbol
// and request_currency_rate) and the host input they read
// (strategy_set_account_currency_fx_scalar, the "account_currency_fx" metadata
// key and strategy_set_account_currency_fx_series).
//
// The rate is a host input. Every rate below is a synthetic number handed to
// the engine through those setters: no feed, corpus or capture is involved.
// The rule under test: the same currency on both sides needs no rate; two
// different currencies convert at the supplied rate in effect at the
// evaluated time; and with no supplied rate the call refuses with
// currency_conversion_rate_unavailable. A refusal case here fails if the call
// returned any number at all, so an identity or a default rate cannot pass it.

// Include order is load-bearing: pineforge.h BEFORE engine.hpp keeps the
// per-strategy declarations visible (engine.hpp defines PINEFORGE_NO_STRATEGY_DECLS).
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#ifndef PINEFORGE_HAS_HOST_CURRENCY_CONVERSION_V1
#error "pine_strategy_host.hpp must advertise the host currency conversions"
#endif

namespace {

using namespace pineforge;

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL [%s] %s:%d  %s\n", g_case, __FILE__,    \
                         __LINE__, #cond);                                     \
        }                                                                      \
    } while (0)

constexpr std::int64_t kStartMs = INT64_C(1704067200000);  // 2024-01-01T00:00:00Z
constexpr std::int64_t kHourMs = INT64_C(3600000);
const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

const char kCode[] = "currency_conversion_rate_unavailable";
const char kNoRate[] = "the host supplied no rate in effect at that time";
const char kOtherPair[] =
    "the host-supplied rate converts only between the symbol currency and the account currency";
const char kBadCode[] = "a currency is missing or invalid";

// A source host whose script body is the case's own.
class ConversionHost final : public source::PineStrategyHost {
public:
    std::function<void(ConversionHost&, const Bar&)> body;
    int calculations = 0;

    void on_source_bar(const Bar& bar) override {
        ++calculations;
        if (body) body(*this, bar);
    }

    // The configuration the cases read back.
    double scalar() const { return account_currency_fx_; }
    bool scalar_supplied() const { return account_currency_fx_scalar_supplied_; }
    // set_syminfo_string ignores an empty value, so a missing code is forced.
    void force_symbol_currency(const std::string& code) { syminfo_.currency = code; }
    // The ledger's numeric scalar alone, as a fixture that writes the member
    // leaves it: a rate nobody supplied through a setter.
    void force_scalar_without_supply(double rate) { account_currency_fx_ = rate; }
};

pf_strategy_t handle(ConversionHost& host) {
    BacktestEngine& engine = host;
    return &engine;
}

void symbol_currency(ConversionHost& host, const char* code) {
    CHECK(strategy_set_syminfo_string(handle(host), "currency", code) == 0);
}

int set_scalar(ConversionHost& host, double rate) {
    return strategy_set_account_currency_fx_scalar(handle(host), rate);
}

std::vector<Bar> tape(int count, int first = 0) {
    std::vector<Bar> bars;
    for (int i = first; i < first + count; ++i) {
        const double price = 100.0 + i;
        bars.push_back({price, price + 0.5, price - 0.5, price, 1000.0, kStartMs + i * kHourMs});
    }
    return bars;
}

void run(ConversionHost& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
}

// What one call did: a number, or the coded refusal it threw.
struct Outcome {
    bool refused = false;
    double value = kNaN;
    std::string code;
    std::string args;
    std::string text;
};

template <typename Call>
Outcome outcome_of(Call&& call) {
    Outcome out;
    try {
        out.value = call();
    } catch (const std::exception& error) {
        out.refused = true;
        const RunFailureValue failure = classify_run_failure(error);
        out.code = run_failure_code_name(failure.code);
        out.args = failure.args ? *failure.args : std::string("{}");
        out.text = error.what();
    }
    return out;
}

// The refusal's canonical arguments and English, spelled here independently of
// the engine's serializer: keys sorted, no whitespace.
std::string refusal_args(const char* function, const char* reason, const std::string& from,
                         const std::string& to, const std::string& account,
                         const std::string& symbol, std::int64_t time_ms) {
    return std::string("{\"account_currency\":\"") + account + "\",\"from\":\"" + from
        + "\",\"function\":\"" + function + "\",\"reason\":\"" + reason
        + "\",\"symbol_currency\":\"" + symbol + "\",\"time_ms\":" + std::to_string(time_ms)
        + ",\"to\":\"" + to + "\"}";
}

std::string refusal_text(const char* call, const char* detail, const std::string& from,
                         const std::string& to, const std::string& account,
                         const std::string& symbol, std::int64_t time_ms) {
    return std::string(call) + ": no account-currency conversion rate for \"" + from
        + "\" to \"" + to + "\" at time " + std::to_string(time_ms) + " (symbol currency \""
        + symbol + "\", account currency \"" + account + "\"): " + detail;
}

struct Expected {
    const char* function;  // the failure's `function` argument
    const char* call;      // the Pine spelling in the English text
    const char* reason;
    const char* detail;
};
const Expected kToAccountNoRate{"convert_to_account", "strategy.convert_to_account",
                                "rate_unavailable", kNoRate};
const Expected kToSymbolNoRate{"convert_to_symbol", "strategy.convert_to_symbol",
                               "rate_unavailable", kNoRate};
const Expected kRequestNoRate{"currency_rate", "request.currency_rate", "rate_unavailable",
                              kNoRate};
const Expected kRequestOtherPair{"currency_rate", "request.currency_rate", "unsupported_pair",
                                 kOtherPair};
const Expected kToAccountBadCode{"convert_to_account", "strategy.convert_to_account",
                                 "invalid_currency", kBadCode};

// The call refused with the reserved code, naming the function, the currencies
// and the evaluated time. A returned number fails the first check.
void expect_refusal(const Outcome& out, const Expected& expected, const std::string& from,
                    const std::string& to, const std::string& account,
                    const std::string& symbol, std::int64_t time_ms) {
    CHECK(out.refused);
    CHECK(out.code == kCode);
    CHECK(out.args == refusal_args(expected.function, expected.reason, from, to, account,
                                   symbol, time_ms));
    CHECK(out.text == refusal_text(expected.call, expected.detail, from, to, account, symbol,
                                   time_ms));
}

std::string warning_array(const std::string& args) {
    return "[{\"args\":" + args + ",\"code\":\"" + kCode + "\"}]";
}

std::string warnings(ConversionHost& host) {
    const char* text = strategy_get_run_warnings(handle(host));
    CHECK(text != nullptr);
    return text ? text : "";
}

// ---------------------------------------------------------------------------

// Different currencies and no host rate: every call refuses. The default 1.0
// the ledger converts at is not a rate the host supplied.
void absent_different_currency_refuses() {
    ConversionHost host;
    symbol_currency(host, "USDT");
    const std::int64_t t = kStartMs;
    CHECK(!host.scalar_supplied());
    CHECK(host.scalar() == 1.0);
    CHECK(!host.has_account_currency_fx_at(t));

    expect_refusal(outcome_of([&] { return host.strategy_convert_to_account(1000.0, "EUR", t); }),
                   kToAccountNoRate, "USDT", "EUR", "EUR", "USDT", t);
    expect_refusal(outcome_of([&] { return host.strategy_convert_to_symbol(1000.0, "EUR", t); }),
                   kToSymbolNoRate, "EUR", "USDT", "EUR", "USDT", t);
    expect_refusal(
        outcome_of([&] { return host.request_currency_rate("USDT", "EUR", "EUR", t); }),
        kRequestNoRate, "USDT", "EUR", "EUR", "USDT", t);
    expect_refusal(
        outcome_of([&] { return host.request_currency_rate("EUR", "USDT", "EUR", t); }),
        kRequestNoRate, "EUR", "USDT", "EUR", "USDT", t);

    // The wire shape, written out once in full.
    const Outcome pinned =
        outcome_of([&] { return host.strategy_convert_to_account(1000.0, "EUR", t); });
    CHECK(pinned.args
          == "{\"account_currency\":\"EUR\",\"from\":\"USDT\",\"function\":\"convert_to_account\","
             "\"reason\":\"rate_unavailable\",\"symbol_currency\":\"USDT\","
             "\"time_ms\":1704067200000,\"to\":\"EUR\"}");
    CHECK(pinned.text
          == "strategy.convert_to_account: no account-currency conversion rate for \"USDT\" to "
             "\"EUR\" at time 1704067200000 (symbol currency \"USDT\", account currency "
             "\"EUR\"): the host supplied no rate in effect at that time");

    // In a run the refusal is the run's coded failure, at the bar that asked,
    // and nothing after it executes.
    ConversionHost running;
    symbol_currency(running, "USDT");
    running.body = [](ConversionHost& self, const Bar& bar) {
        const double units =
            self.strategy_convert_to_account(1000.0, "EUR", bar.timestamp) / 100000.0;
        self.strategy_entry("Long", true, kNaN, kNaN, units);
    };
    const auto bars = tape(3);
    run(running, bars);
    const pf_strategy_t h = handle(running);
    CHECK(strategy_last_run_status(h) == 1);
    CHECK(std::string(strategy_get_last_error_code(h)) == kCode);
    CHECK(std::string(strategy_get_last_error_args(h))
          == refusal_args("convert_to_account", "rate_unavailable", "USDT", "EUR", "EUR", "USDT",
                          bars[0].timestamp));
    CHECK(std::string(strategy_get_last_error(h))
          == refusal_text("strategy.convert_to_account", kNoRate, "USDT", "EUR", "EUR", "USDT",
                          bars[0].timestamp));
    CHECK(running.calculations == 1);
    CHECK(running.trade_count() == 0);
}

// An explicit 1.0 is host data: the calls answer from it, where the default
// 1.0 beside it refuses. Both setters install through the same rule.
void explicit_scalar_one_is_present() {
    const std::int64_t t = kStartMs;
    ConversionHost host;
    symbol_currency(host, "USDT");
    CHECK(set_scalar(host, 1.0) == 0);
    CHECK(host.scalar_supplied());
    CHECK(host.scalar() == 1.0);
    CHECK(host.has_account_currency_fx_at(t));
    const Outcome to_account =
        outcome_of([&] { return host.strategy_convert_to_account(1000.0, "EUR", t); });
    CHECK(!to_account.refused);
    CHECK(to_account.value == 1000.0);
    const Outcome to_symbol =
        outcome_of([&] { return host.strategy_convert_to_symbol(1000.0, "EUR", t); });
    CHECK(!to_symbol.refused);
    CHECK(to_symbol.value == 1000.0);
    CHECK(host.request_currency_rate("USDT", "EUR", "EUR", t) == 1.0);
    CHECK(host.request_currency_rate("EUR", "USDT", "EUR", t) == 1.0);

    ConversionHost by_metadata;
    symbol_currency(by_metadata, "USDT");
    strategy_set_syminfo_metadata(handle(by_metadata), "account_currency_fx", 1.0);
    CHECK(by_metadata.scalar_supplied());
    CHECK(by_metadata.has_account_currency_fx_at(t));
    CHECK(by_metadata.strategy_convert_to_account(1000.0, "EUR", t) == 1000.0);

    // The same number, never supplied.
    ConversionHost fresh;
    symbol_currency(fresh, "USDT");
    CHECK(fresh.scalar() == 1.0);
    CHECK(!fresh.scalar_supplied());
    CHECK(outcome_of([&] { return fresh.strategy_convert_to_account(1000.0, "EUR", t); })
              .refused);
}

// r = account units per symbol unit: to the account multiplies, to the symbol
// divides. The second half is the reported case's flat 0.9.
void scalar_multiply_divide() {
    const std::int64_t t = kStartMs;
    ConversionHost host;
    symbol_currency(host, "USDT");
    CHECK(set_scalar(host, 1.25) == 0);
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", t) == 1250.0);
    CHECK(host.strategy_convert_to_symbol(1000.0, "EUR", t) == 800.0);
    CHECK(host.strategy_convert_to_account(-4.0, "EUR", t) == -5.0);
    CHECK(host.strategy_convert_to_account(0.0, "EUR", t) == 0.0);
    CHECK(std::isnan(host.strategy_convert_to_account(kNaN, "EUR", t)));
    CHECK(std::isnan(host.strategy_convert_to_symbol(kNaN, "EUR", t)));

    ConversionHost reported;
    symbol_currency(reported, "USDT");
    strategy_set_syminfo_metadata(handle(reported), "account_currency_fx", 0.9);
    CHECK(reported.scalar_supplied());
    const double units = reported.strategy_convert_to_account(1000.0, "EUR", t) / 100000.0;
    CHECK(units == 1000.0 * 0.9 / 100000.0);
    CHECK(std::fabs(units - 0.009) < 1e-15);
    CHECK(units != 1000.0 / 100000.0);  // not the unconverted 0.01
    CHECK(reported.strategy_convert_to_symbol(900.0, "EUR", t) == 900.0 / 0.9);
}

// request.currency_rate answers the symbol -> account rate and its reciprocal,
// and nothing else: no other pair, no triangulation, no similar code.
void forward_reverse_pair() {
    const std::int64_t t = kStartMs;
    ConversionHost host;
    symbol_currency(host, "USDT");
    CHECK(set_scalar(host, 1.25) == 0);
    const double forward = host.request_currency_rate("USDT", "EUR", "EUR", t);
    const double reverse = host.request_currency_rate("EUR", "USDT", "EUR", t);
    CHECK(forward == 1.25);
    CHECK(reverse == 0.8);
    CHECK(reverse == 1.0 / forward);
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", t) == 1000.0 * forward);
    CHECK(host.strategy_convert_to_symbol(1000.0, "EUR", t) == 1000.0 / forward);

    const struct {
        const char* from;
        const char* to;
    } other_pairs[] = {
        {"GBP", "EUR"},    // another currency into the account's
        {"USDT", "GBP"},   // the symbol's into another
        {"GBP", "JPY"},    // neither side
        {"USD", "EUR"},    // USDT is not USD
        {"USDT", "USDT"},  // not the account == symbol identity
        {"EUR", "EUR"},
    };
    for (const auto& pair : other_pairs) {
        expect_refusal(outcome_of([&] {
                           return host.request_currency_rate(pair.from, pair.to, "EUR", t);
                       }),
                       kRequestOtherPair, pair.from, pair.to, "EUR", "USDT", t);
    }
}

// The same currency on both sides is the one case that needs no host input,
// and it is decided by the codes, never by a rate.
void same_currency_without_input_identity() {
    const std::int64_t t = kStartMs;
    ConversionHost host;
    symbol_currency(host, "USD");
    CHECK(!host.has_account_currency_fx_at(t));
    CHECK(host.strategy_convert_to_account(1234.5, "USD", t) == 1234.5);
    CHECK(host.strategy_convert_to_symbol(1234.5, "USD", t) == 1234.5);
    CHECK(host.request_currency_rate("USD", "USD", "USD", t) == 1.0);
    CHECK(std::isnan(host.strategy_convert_to_account(kNaN, "USD", t)));

    // A supplied rate that is not 1 does not touch the same currency.
    CHECK(set_scalar(host, 3.0) == 0);
    CHECK(host.strategy_convert_to_account(1234.5, "USD", t) == 1234.5);
    CHECK(host.strategy_convert_to_symbol(1234.5, "USD", t) == 1234.5);
    CHECK(host.request_currency_rate("USD", "USD", "USD", t) == 1.0);

    // A similar code is another currency.
    ConversionHost stable;
    symbol_currency(stable, "USDT");
    expect_refusal(outcome_of([&] { return stable.strategy_convert_to_account(1.0, "USD", t); }),
                   kToAccountNoRate, "USDT", "USD", "USD", "USDT", t);
    ConversionHost lower;
    symbol_currency(lower, "USD");
    expect_refusal(outcome_of([&] { return lower.strategy_convert_to_account(1.0, "usd", t); }),
                   kToAccountNoRate, "USD", "usd", "usd", "USD", t);

    // Two missing or invalid codes are equal texts and still no currency.
    ConversionHost missing;
    missing.force_symbol_currency("");
    expect_refusal(outcome_of([&] { return missing.strategy_convert_to_account(1.0, "", t); }),
                   kToAccountBadCode, "", "", "", "", t);
    ConversionHost none;
    none.force_symbol_currency("NONE");
    expect_refusal(outcome_of([&] { return none.strategy_convert_to_account(1.0, "NONE", t); }),
                   kToAccountBadCode, "NONE", "NONE", "NONE", "NONE", t);
    expect_refusal(outcome_of([&] { return host.strategy_convert_to_account(1.0, "US D", t); }),
                   kToAccountBadCode, "USD", "US D", "US D", "USD", t);
}

// A curve point is in effect from its own timestamp on. Before the first
// point nothing is: a curve that only starts later supplies no rate now, and
// the evaluated time is never moved to reach one.
void curve_effective_boundary_and_future_only_absence() {
    ConversionHost host;
    symbol_currency(host, "USDT");
    const std::int64_t first = kStartMs + 10 * kHourMs;
    const std::int64_t second = kStartMs + 20 * kHourMs;
    const std::int64_t stamps[] = {first, second};
    const double rates[] = {1.25, 2.0};
    CHECK(strategy_set_account_currency_fx_series(handle(host), stamps, rates, 2) == 0);
    CHECK(!host.scalar_supplied());

    CHECK(!host.has_account_currency_fx_at(first - 1));
    expect_refusal(
        outcome_of([&] { return host.strategy_convert_to_account(1000.0, "EUR", first - 1); }),
        kToAccountNoRate, "USDT", "EUR", "EUR", "USDT", first - 1);
    expect_refusal(
        outcome_of([&] { return host.request_currency_rate("USDT", "EUR", "EUR", kStartMs); }),
        kRequestNoRate, "USDT", "EUR", "EUR", "USDT", kStartMs);

    CHECK(host.has_account_currency_fx_at(first));
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", first) == 1250.0);
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", second - 1) == 1250.0);
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", second) == 2000.0);
    CHECK(host.strategy_convert_to_symbol(1000.0, "EUR", second) == 500.0);
    CHECK(host.request_currency_rate("USDT", "EUR", "EUR", second) == 2.0);
    CHECK(host.request_currency_rate("EUR", "USDT", "EUR", second) == 0.5);
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", second + 1000 * kHourMs) == 2000.0);
}

// Clearing the curve (n = 0) removes the curve and nothing else.
void clear_curve_preserves_explicit_scalar() {
    const std::int64_t point = kStartMs + 10 * kHourMs;
    const std::int64_t stamps[] = {point};
    const double rates[] = {2.0};

    ConversionHost host;
    symbol_currency(host, "USDT");
    CHECK(set_scalar(host, 0.5) == 0);
    CHECK(strategy_set_account_currency_fx_series(handle(host), stamps, rates, 1) == 0);
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", point - 1) == 500.0);
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", point) == 2000.0);
    CHECK(strategy_set_account_currency_fx_series(handle(host), nullptr, nullptr, 0) == 0);
    CHECK(host.scalar_supplied());
    CHECK(host.scalar() == 0.5);
    CHECK(host.has_account_currency_fx_at(point));
    CHECK(host.strategy_convert_to_account(1000.0, "EUR", point) == 500.0);

    // With no scalar the cleared curve leaves no rate at all.
    ConversionHost curve_only;
    symbol_currency(curve_only, "USDT");
    CHECK(strategy_set_account_currency_fx_series(handle(curve_only), stamps, rates, 1) == 0);
    CHECK(curve_only.strategy_convert_to_account(1000.0, "EUR", point) == 2000.0);
    CHECK(strategy_set_account_currency_fx_series(handle(curve_only), nullptr, nullptr, 0) == 0);
    CHECK(!curve_only.has_account_currency_fx_at(point));
    expect_refusal(
        outcome_of([&] { return curve_only.strategy_convert_to_account(1000.0, "EUR", point); }),
        kToAccountNoRate, "USDT", "EUR", "EUR", "USDT", point);
}

// An invalid scalar is refused and takes the earlier one with it: the ledger
// falls back to 1.0, and that fallback answers no conversion.
void invalid_scalar_clears_presence() {
    const std::int64_t t = kStartMs;
    for (const double invalid : {0.0, -1.0, kNaN, kInf, -kInf}) {
        ConversionHost host;
        symbol_currency(host, "USDT");
        CHECK(set_scalar(host, 0.5) == 0);
        CHECK(host.strategy_convert_to_account(1000.0, "EUR", t) == 500.0);
        CHECK(set_scalar(host, invalid) == -1);
        CHECK(!host.scalar_supplied());
        CHECK(host.scalar() == 1.0);
        CHECK(!host.has_account_currency_fx_at(t));
        expect_refusal(
            outcome_of([&] { return host.strategy_convert_to_account(1000.0, "EUR", t); }),
            kToAccountNoRate, "USDT", "EUR", "EUR", "USDT", t);
        CHECK(set_scalar(host, 2.0) == 0);
        CHECK(host.strategy_convert_to_account(1000.0, "EUR", t) == 2000.0);
    }

    ConversionHost by_metadata;
    symbol_currency(by_metadata, "USDT");
    strategy_set_syminfo_metadata(handle(by_metadata), "account_currency_fx", 0.5);
    CHECK(by_metadata.strategy_convert_to_account(1000.0, "EUR", t) == 500.0);
    strategy_set_syminfo_metadata(handle(by_metadata), "account_currency_fx", kNaN);
    CHECK(!by_metadata.scalar_supplied());
    CHECK(by_metadata.scalar() == 1.0);
    expect_refusal(
        outcome_of([&] { return by_metadata.strategy_convert_to_account(1000.0, "EUR", t); }),
        kToAccountNoRate, "USDT", "EUR", "EUR", "USDT", t);
}

// A refused curve changes nothing: the earlier curve keeps answering, and a
// host that had none still has none.
void invalid_curve_transaction() {
    const std::int64_t first = kStartMs + 10 * kHourMs;
    const std::int64_t second = kStartMs + 20 * kHourMs;
    const std::int64_t stamps[] = {first, second};
    const double rates[] = {1.25, 2.0};
    const std::int64_t unsorted[] = {second, first};
    const std::int64_t duplicate[] = {first, first};
    const double with_nan[] = {1.5, kNaN};
    const double with_zero[] = {1.5, 0.0};
    const double with_negative[] = {1.5, -2.0};
    const double replacement[] = {5.0, 6.0};

    ConversionHost host;
    symbol_currency(host, "USDT");
    const pf_strategy_t h = handle(host);
    CHECK(strategy_set_account_currency_fx_series(h, stamps, rates, 2) == 0);
    const auto unchanged = [&] {
        CHECK(host.strategy_convert_to_account(1000.0, "EUR", first) == 1250.0);
        CHECK(host.strategy_convert_to_account(1000.0, "EUR", second) == 2000.0);
        CHECK(!host.has_account_currency_fx_at(first - 1));
    };
    CHECK(strategy_set_account_currency_fx_series(h, unsorted, replacement, 2) == -1);
    unchanged();
    CHECK(strategy_set_account_currency_fx_series(h, duplicate, replacement, 2) == -1);
    unchanged();
    CHECK(strategy_set_account_currency_fx_series(h, stamps, with_nan, 2) == -1);
    unchanged();
    CHECK(strategy_set_account_currency_fx_series(h, stamps, with_zero, 2) == -1);
    unchanged();
    CHECK(strategy_set_account_currency_fx_series(h, stamps, with_negative, 2) == -1);
    unchanged();
    CHECK(strategy_set_account_currency_fx_series(h, nullptr, replacement, 2) == -1);
    unchanged();
    CHECK(strategy_set_account_currency_fx_series(h, stamps, nullptr, 2) == -1);
    unchanged();
    CHECK(strategy_set_account_currency_fx_series(h, stamps, replacement, -1) == -1);
    unchanged();

    ConversionHost empty;
    symbol_currency(empty, "USDT");
    CHECK(strategy_set_account_currency_fx_series(handle(empty), stamps, with_nan, 2) == -1);
    CHECK(!empty.has_account_currency_fx_at(second));
    expect_refusal(
        outcome_of([&] { return empty.strategy_convert_to_account(1000.0, "EUR", second); }),
        kToAccountNoRate, "USDT", "EUR", "EUR", "USDT", second);
}

// ignore_invalid_currency: na instead of a stop, and the run says so.
void ignored_invalid_returns_na_with_diagnostic() {
    ConversionHost host;
    symbol_currency(host, "USDT");
    std::vector<double> seen;
    host.body = [&seen](ConversionHost& self, const Bar& bar) {
        seen.push_back(self.request_currency_rate("USDT", "EUR", "EUR", bar.timestamp, true));
    };
    const auto bars = tape(6);
    run(host, bars);
    CHECK(host.last_run_status() == 0);
    CHECK(host.last_error().empty());
    CHECK(seen.size() == 6);
    for (const double value : seen) CHECK(std::isnan(value));
    CHECK(warnings(host)
          == warning_array(refusal_args("currency_rate", "rate_unavailable", "USDT", "EUR", "EUR",
                                        "USDT", bars[0].timestamp)));
    CHECK(warnings(host)
          == "[{\"args\":{\"account_currency\":\"EUR\",\"from\":\"USDT\","
             "\"function\":\"currency_rate\",\"reason\":\"rate_unavailable\","
             "\"symbol_currency\":\"USDT\",\"time_ms\":1704067200000,\"to\":\"EUR\"},"
             "\"code\":\"currency_conversion_rate_unavailable\"}]");

    // A pair the host's rate does not join is na with its own reason, even
    // with a rate supplied.
    ConversionHost other;
    symbol_currency(other, "USDT");
    CHECK(set_scalar(other, 1.25) == 0);
    std::vector<double> other_seen;
    other.body = [&other_seen](ConversionHost& self, const Bar& bar) {
        other_seen.push_back(
            self.request_currency_rate("GBP", "JPY", "EUR", bar.timestamp, true));
    };
    run(other, bars);
    CHECK(other.last_run_status() == 0);
    CHECK(other_seen.size() == 6);
    for (const double value : other_seen) CHECK(std::isnan(value));
    CHECK(warnings(other)
          == warning_array(refusal_args("currency_rate", "unsupported_pair", "GBP", "JPY", "EUR",
                                        "USDT", bars[0].timestamp)));

    // Without the flag the same request stops the run and records no warning.
    ConversionHost strict;
    symbol_currency(strict, "USDT");
    strict.body = [](ConversionHost& self, const Bar& bar) {
        (void)self.request_currency_rate("USDT", "EUR", "EUR", bar.timestamp, false);
    };
    run(strict, bars);
    CHECK(strict.last_run_status() == 1);
    CHECK(std::string(strategy_get_last_error_code(handle(strict))) == kCode);
    CHECK(std::string(strategy_get_last_error_args(handle(strict)))
          == refusal_args("currency_rate", "rate_unavailable", "USDT", "EUR", "EUR", "USDT",
                          bars[0].timestamp));
    CHECK(strict.calculations == 1);
    CHECK(warnings(strict) == "[]");
}

// The warning is one record however many bars repeat the request, a run
// starts with none, and the supplied mark survives a rerun untouched.
void rerun_reset_and_diagnostic_dedup() {
    ConversionHost host;
    symbol_currency(host, "USDT");
    std::vector<double> seen;
    host.body = [&seen](ConversionHost& self, const Bar& bar) {
        seen.push_back(self.request_currency_rate("USDT", "EUR", "EUR", bar.timestamp, true));
    };
    const auto bars = tape(8);

    run(host, bars);
    CHECK(host.last_run_status() == 0);
    CHECK(seen.size() == 8);
    for (const double value : seen) CHECK(std::isnan(value));
    CHECK(warnings(host)
          == warning_array(refusal_args("currency_rate", "rate_unavailable", "USDT", "EUR", "EUR",
                                        "USDT", bars[0].timestamp)));

    // A supplied rate: the next run answers from it and warns of nothing.
    CHECK(set_scalar(host, 1.25) == 0);
    seen.clear();
    run(host, bars);
    CHECK(host.last_run_status() == 0);
    CHECK(seen.size() == 8);
    for (const double value : seen) CHECK(value == 1.25);
    CHECK(warnings(host) == "[]");
    CHECK(host.scalar_supplied());

    // A rerun with the configuration untouched: the rate is still supplied.
    seen.clear();
    run(host, bars);
    CHECK(host.last_run_status() == 0);
    CHECK(seen.size() == 8);
    for (const double value : seen) CHECK(value == 1.25);
    CHECK(warnings(host) == "[]");
    CHECK(host.scalar_supplied());
    CHECK(host.scalar() == 1.25);

    // Cleared again: the warning is back, once, and it is this run's.
    CHECK(set_scalar(host, kNaN) == -1);
    const auto later = tape(8, 100);
    seen.clear();
    run(host, later);
    CHECK(host.last_run_status() == 0);
    CHECK(seen.size() == 8);
    for (const double value : seen) CHECK(std::isnan(value));
    CHECK(warnings(host)
          == warning_array(refusal_args("currency_rate", "rate_unavailable", "USDT", "EUR", "EUR",
                                        "USDT", later[0].timestamp)));
}

struct BarAnswer {
    std::int64_t time_ms;
    bool has_rate;
    Outcome outcome;
};

// A bare native host: its scalar is the one its run specification declares
// and its curve the immutable one configure_native_fx_curve stages.
struct NativeProbeHost final : NativeStrategyHost {
    struct Row {
        std::int64_t time_ms;
        bool has_rate;
        double ledger_rate;
    };
    std::vector<Row> rows;

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        rows.push_back({bar.timestamp, has_account_currency_fx_at(bar.timestamp),
                        account_currency_fx_at(bar.timestamp)});
    }
};

// Inside a run the curve is the run's own immutable copy and the scalar is
// the one its specification carries. Neither turns an unsupplied 1.0 into a
// rate: before the curve's first point only an explicit scalar answers.
void immutable_native_curve_presence() {
    const auto bars = tape(6);
    const std::int64_t point = bars[3].timestamp;
    const std::int64_t stamps[] = {point};
    const double rates[] = {1.25};
    const auto record = [](std::vector<BarAnswer>& answers) {
        return [&answers](ConversionHost& self, const Bar& bar) {
            answers.push_back(
                {bar.timestamp, self.has_account_currency_fx_at(bar.timestamp),
                 outcome_of([&] {
                     return self.strategy_convert_to_account(1000.0, "EUR", bar.timestamp);
                 })});
        };
    };

    ConversionHost curve_only;
    symbol_currency(curve_only, "USDT");
    CHECK(strategy_set_account_currency_fx_series(handle(curve_only), stamps, rates, 1) == 0);
    std::vector<BarAnswer> answers;
    curve_only.body = record(answers);
    run(curve_only, bars);
    CHECK(curve_only.last_run_status() == 0);
    CHECK(curve_only.last_error().empty());
    CHECK(answers.size() == 6);
    for (std::size_t i = 0; i < answers.size(); ++i) {
        CHECK(answers[i].time_ms == bars[i].timestamp);
        if (i < 3) {
            CHECK(!answers[i].has_rate);
            expect_refusal(answers[i].outcome, kToAccountNoRate, "USDT", "EUR", "EUR", "USDT",
                           bars[i].timestamp);
        } else {
            CHECK(answers[i].has_rate);
            CHECK(!answers[i].outcome.refused);
            CHECK(answers[i].outcome.value == 1250.0);
        }
    }
    CHECK(!curve_only.scalar_supplied());
    CHECK(curve_only.scalar() == 1.0);
    CHECK(!curve_only.has_account_currency_fx_at(bars[0].timestamp));
    CHECK(curve_only.has_account_currency_fx_at(point));

    ConversionHost with_scalar;
    symbol_currency(with_scalar, "USDT");
    CHECK(set_scalar(with_scalar, 0.5) == 0);
    CHECK(strategy_set_account_currency_fx_series(handle(with_scalar), stamps, rates, 1) == 0);
    std::vector<BarAnswer> scalar_answers;
    with_scalar.body = record(scalar_answers);
    run(with_scalar, bars);
    CHECK(with_scalar.last_run_status() == 0);
    CHECK(scalar_answers.size() == 6);
    for (std::size_t i = 0; i < scalar_answers.size(); ++i) {
        CHECK(scalar_answers[i].has_rate);
        CHECK(!scalar_answers[i].outcome.refused);
        CHECK(scalar_answers[i].outcome.value == (i < 3 ? 500.0 : 1250.0));
    }
    CHECK(with_scalar.scalar_supplied());
    CHECK(with_scalar.scalar() == 0.5);

    // The same on a bare native host. Its run specification declares a scalar
    // of its own (0.9) and its curve is staged beside it: the ledger converts
    // at 0.9 before the curve's first point, and that number is still not a
    // rate a host supplied, so nothing is in effect there for a conversion.
    NativeProbeHost native;
    NativeRunSpec spec;
    spec.identity = {"host-currency-conversion", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:FX";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 0.9;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    spec.event_retention = NativeEventRetention::Full;
    CHECK(native.configure_native(spec).status == NativeSetupStatus::Applied);
    std::vector<Bar> minutes;
    for (int i = 0; i < 4; ++i)
        minutes.push_back({100.0, 100.0, 100.0, 100.0, 1.0, kStartMs + i * INT64_C(60000)});
    NativeFxCurve curve;
    curve.effective_from_ms = {minutes[2].timestamp};
    curve.account_per_quote = {1.25};
    CHECK(native.configure_native_fx_curve(curve).status == NativeSetupStatus::Applied);
    native.run(minutes.data(), static_cast<int>(minutes.size()));
    CHECK(native.last_error().empty());
    CHECK(native.rows.size() == 4);
    for (std::size_t i = 0; i < native.rows.size(); ++i) {
        CHECK(native.rows[i].time_ms == minutes[i].timestamp);
        CHECK(native.rows[i].has_rate == (i >= 2));
        CHECK(native.rows[i].ledger_rate == (i >= 2 ? 1.25 : 0.9));
    }
}

// The scalar is run configuration: once a stream's warmup has begun the
// setter is refused and changes nothing, before and after the first tick.
void stream_warmup_rejection() {
    pf_bar_t warmup{};
    warmup.open = warmup.high = warmup.low = warmup.close = 100.0;
    warmup.volume = 2.0;
    warmup.timestamp = 0;
    pf_trade_tick_t tick{};
    tick.timestamp = 60010;
    tick.sequence = 7;
    tick.price = 101.0;
    tick.quantity = 0.5;
    const auto record = [](std::vector<Outcome>& outcomes) {
        return [&outcomes](ConversionHost& self, const Bar& bar) {
            outcomes.push_back(outcome_of([&] {
                return self.strategy_convert_to_account(1000.0, "EUR", bar.timestamp);
            }));
        };
    };

    ConversionHost late;
    symbol_currency(late, "USDT");
    std::vector<Outcome> late_outcomes;
    late.body = record(late_outcomes);
    const pf_strategy_t h = handle(late);
    CHECK(strategy_stream_begin(h, &warmup, 1, "1", "1") == 0);
    CHECK(set_scalar(late, 0.5) == -1);
    CHECK(!late.scalar_supplied());
    CHECK(late.scalar() == 1.0);
    CHECK(strategy_stream_push_tick(h, &tick) == 0);
    CHECK(set_scalar(late, 0.5) == -1);
    CHECK(!late.scalar_supplied());
    CHECK(late.scalar() == 1.0);
    CHECK(strategy_stream_advance_time(h, 120000) == 0);
    CHECK(late.last_error().empty());
    CHECK(!late_outcomes.empty());
    for (const Outcome& outcome : late_outcomes) {
        CHECK(outcome.refused);
        CHECK(outcome.code == kCode);
    }
    CHECK(strategy_stream_end(h, 0) == 0);

    // A scalar installed before the stream began is the stream's rate, and a
    // later call cannot replace it.
    ConversionHost primed;
    symbol_currency(primed, "USDT");
    std::vector<Outcome> primed_outcomes;
    primed.body = record(primed_outcomes);
    const pf_strategy_t p = handle(primed);
    CHECK(set_scalar(primed, 0.5) == 0);
    CHECK(strategy_stream_begin(p, &warmup, 1, "1", "1") == 0);
    CHECK(set_scalar(primed, 2.0) == -1);
    CHECK(strategy_stream_push_tick(p, &tick) == 0);
    CHECK(set_scalar(primed, 2.0) == -1);
    CHECK(strategy_stream_advance_time(p, 120000) == 0);
    CHECK(primed.last_error().empty());
    CHECK(primed.scalar_supplied());
    CHECK(primed.scalar() == 0.5);
    CHECK(!primed_outcomes.empty());
    for (const Outcome& outcome : primed_outcomes) {
        CHECK(!outcome.refused);
        CHECK(outcome.value == 500.0);
    }
    CHECK(strategy_stream_end(p, 0) == 0);
}

std::uint64_t hash_after_run(ConversionHost& host) {
    const auto bars = tape(4);
    run(host, bars);
    CHECK(host.last_run_status() == 0);
    CHECK(host.last_error().empty());
    return host.broker_state_hash();
}

// Whether the host supplied the scalar is configuration the hash folds: an
// explicit 1.0 and the default 1.0 are different runs, a run that supplies
// none hashes as one that never could, and the same rate hashes apart by
// whether it was supplied.
void source_state_hash_presence_coverage() {
    ConversionHost absent, absent_twin, explicit_one, metadata_one, cleared;
    for (ConversionHost* host : {&absent, &absent_twin, &explicit_one, &metadata_one, &cleared})
        symbol_currency(*host, "USDT");
    CHECK(set_scalar(explicit_one, 1.0) == 0);
    strategy_set_syminfo_metadata(handle(metadata_one), "account_currency_fx", 1.0);
    CHECK(set_scalar(cleared, 1.0) == 0);
    CHECK(set_scalar(cleared, kNaN) == -1);

    const std::uint64_t h_absent = hash_after_run(absent);
    const std::uint64_t h_one = hash_after_run(explicit_one);
    CHECK(h_absent == hash_after_run(absent_twin));
    CHECK(h_one != h_absent);
    CHECK(hash_after_run(metadata_one) == h_one);
    CHECK(hash_after_run(cleared) == h_absent);

    ConversionHost supplied_two, forced_two;
    symbol_currency(supplied_two, "USDT");
    symbol_currency(forced_two, "USDT");
    CHECK(set_scalar(supplied_two, 2.0) == 0);
    forced_two.force_scalar_without_supply(2.0);
    CHECK(supplied_two.scalar() == forced_two.scalar());
    const std::uint64_t h_supplied_two = hash_after_run(supplied_two);
    CHECK(h_supplied_two != hash_after_run(forced_two));
    CHECK(h_supplied_two != h_one);
}

}  // namespace

int main() {
    const struct {
        const char* name;
        void (*run)();
    } cases[] = {
        {"absent_different_currency_refuses", absent_different_currency_refuses},
        {"explicit_scalar_one_is_present", explicit_scalar_one_is_present},
        {"scalar_multiply_divide", scalar_multiply_divide},
        {"forward_reverse_pair", forward_reverse_pair},
        {"same_currency_without_input_identity", same_currency_without_input_identity},
        {"curve_effective_boundary_and_future_only_absence",
         curve_effective_boundary_and_future_only_absence},
        {"clear_curve_preserves_explicit_scalar", clear_curve_preserves_explicit_scalar},
        {"invalid_scalar_clears_presence", invalid_scalar_clears_presence},
        {"invalid_curve_transaction", invalid_curve_transaction},
        {"ignored_invalid_returns_na_with_diagnostic",
         ignored_invalid_returns_na_with_diagnostic},
        {"rerun_reset_and_diagnostic_dedup", rerun_reset_and_diagnostic_dedup},
        {"immutable_native_curve_presence", immutable_native_curve_presence},
        {"stream_warmup_rejection", stream_warmup_rejection},
        {"source_state_hash_presence_coverage", source_state_hash_presence_coverage},
    };
    for (const auto& test_case : cases) {
        g_case = test_case.name;
        const int before = g_failures;
        try {
            test_case.run();
        } catch (const std::exception& error) {
            ++g_failures;
            std::fprintf(stderr, "FAIL [%s] unexpected exception: %s\n", g_case, error.what());
        }
        std::printf("%s: %s\n", test_case.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("test_host_currency_conversion: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
