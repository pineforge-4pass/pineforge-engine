// test_chart_ema_batch_security_policy -- the chart EMA seeding and the
// warm-up of a request.security series are two policies, and neither moves
// the other: the chart's default (na, then the simple average) holds whatever
// the request.security policy is, and the request.security series is as it
// is with nothing declared whatever the chart's seeding is.
//
// SecurityStrategy holds a chart ta.ema (the generated shape of
// test_chart_ema_generated_cache_seed, cached in a batch run) and the EMA of a
// request.security("60", ta.ema(close, 3)) on a 15-minute chart. Seven
// declarations are run on the batch path and on the per-bar path:
//
//   nothing                                  chart SMA seed,    security first value
//   chart_ema_na_warmup = 1                  chart SMA seed,    security as with nothing
//   chart_ema_na_warmup = 0                  chart first value, security as with nothing
//   series_origin_ms, then
//     security_range_start_na_warmup = 0     chart SMA seed,    security as with nothing
//   series_origin_ms                         chart SMA seed,    security SMA seed
//   security_range_start_na_warmup           chart SMA seed,    security SMA seed
//   security_range_start_na_warmup, then
//     chart_ema_na_warmup = 0                chart first value, security SMA seed
//
// The fourth is how a caller declares a reference origin without the
// request.security warm-up.
// The rows read the seeding default inside the security evaluation and inside
// the chart callback, the completed hourly bars, and the security EMA over
// them, and the chart EMA on the cache and per bar.
//
// Public synthetic bars only: no TradingView measurement and no private data.
// The EMA expectations come from the definition (oracle_ema) over the hourly
// closes, which are checked against the feed.

#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;
int checks = 0;
std::string row;

void expect(bool ok, const std::string& what) {
    ++checks;
    if (ok) return;
    std::printf("FAIL [%s] %s\n", row.c_str(), what.c_str());
    ++failures;
}

void expect_int(long long observed, long long expected, const std::string& what) {
    ++checks;
    if (observed == expected) return;
    std::printf("FAIL [%s] %s: observed %lld, expected %lld\n", row.c_str(), what.c_str(),
                observed, expected);
    ++failures;
}

bool same(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

bool near(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b));
}

void expect_series(const std::vector<double>& observed, const std::vector<double>& expected,
                   bool exact, const std::string& what) {
    ++checks;
    if (observed.size() != expected.size()) {
        std::printf("FAIL [%s] %s: observed %zu values, expected %zu\n", row.c_str(),
                    what.c_str(), observed.size(), expected.size());
        ++failures;
        return;
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < observed.size(); ++i) {
        if (exact ? same(observed[i], expected[i]) : near(observed[i], expected[i])) continue;
        if (++differing <= 4)
            std::printf("FAIL [%s] %s: value %zu observed %.17g, expected %.17g\n", row.c_str(),
                        what.c_str(), i, observed[i], expected[i]);
    }
    if (differing == 0) return;
    std::printf("FAIL [%s] %s: %zu of %zu values differ\n", row.c_str(), what.c_str(), differing,
                observed.size());
    ++failures;
}

int first_finite(const std::vector<double>& values) {
    for (std::size_t i = 0; i < values.size(); ++i)
        if (!std::isnan(values[i])) return static_cast<int>(i);
    return -1;
}

int count_of(const std::vector<int>& flags, int value) {
    int count = 0;
    for (const int flag : flags)
        if (flag == value) ++count;
    return count;
}

// Definition only. sma_seed: na for length - 1 inputs, then the simple average
// of the first `length` inputs, then the recursion. Otherwise the first input
// seeds the recursion.
std::vector<double> oracle_ema(const std::vector<double>& inputs, int length, bool sma_seed) {
    std::vector<double> values(inputs.size(), na<double>());
    const double alpha = 2.0 / (static_cast<double>(length) + 1.0);
    double previous = na<double>();
    double sum = 0.0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const double input = inputs[i];
        if (sma_seed && i < static_cast<std::size_t>(length)) {
            sum += input;
            if (i + 1 == static_cast<std::size_t>(length))
                previous = sum / static_cast<double>(length);
        } else if (!sma_seed && i == 0) {
            previous = input;
        } else {
            previous = previous + alpha * (input - previous);
        }
        values[i] = previous;
    }
    return values;
}

constexpr int kLength = 3;
constexpr std::int64_t kQuarter = 15 * 60'000;
constexpr std::int64_t kHour = 4 * kQuarter;
constexpr std::int64_t kStart = 1'704'067'200'000;  // 2024-01-01T00:00:00Z

class SecurityStrategy final : public source::PineStrategyHost {
public:
    // The chart's `x = ta.ema(close, 3)`, in the generated shape.
    ta::EMA _ta_ema_1;
    std::vector<double> _precalc__ta_ema_1;
    bool _use_precalc = false;
    double x = 0.0;
    // The EMA inside request.security("60", ta.ema(close, 3)).
    ta::EMA _sec_ema_1;

    // What the rows read back; no generated member.
    int preparations = 0;
    bool allowed = false;
    bool seeding_in_preparation = false;
    int cache_reads = 0;
    int per_bar_computes = 0;
    std::vector<double> chart_seen;
    std::vector<int> chart_flags;
    int partial_evaluations = 0;
    int other_evaluations = 0;
    std::vector<std::int64_t> security_opens;
    std::vector<double> security_closes;
    std::vector<double> security_values;
    std::vector<int> security_flags;

    SecurityStrategy() : _ta_ema_1(kLength), _sec_ema_1(kLength) {
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        register_security_eval(0, "60", "15", false, false);
    }

    void prepare_script_run(const Bar* bars, int n, bool allow_precalculation) override {
        ++preparations;
        allowed = allow_precalculation;
        seeding_in_preparation = ta::ema_na_warmup_flag();
        cache_reads = 0;
        per_bar_computes = 0;
        chart_seen.clear();
        chart_flags.clear();
        partial_evaluations = 0;
        other_evaluations = 0;
        security_opens.clear();
        security_closes.clear();
        security_values.clear();
        security_flags.clear();
        this->_ta_ema_1 = decltype(this->_ta_ema_1)(kLength);
        this->_precalc__ta_ema_1 = decltype(this->_precalc__ta_ema_1){};
        this->_use_precalc = false;
        this->x = 0.0;
        this->_sec_ema_1 = decltype(this->_sec_ema_1)(kLength);
        if (allow_precalculation) precalculate(bars, n);
    }

    void evaluate_security(int id, const Bar& bar, bool complete) override {
        if (id != 0) {
            ++other_evaluations;
            return;
        }
        if (!complete) {
            ++partial_evaluations;
            return;
        }
        security_opens.push_back(bar.timestamp);
        security_closes.push_back(bar.close);
        security_flags.push_back(ta::ema_na_warmup_flag() ? 1 : 0);
        security_values.push_back(_sec_ema_1.compute(bar.close));
    }

    void on_source_bar(const Bar& bar) override {
        (void)bar;
        chart_flags.push_back(ta::ema_na_warmup_flag() ? 1 : 0);
        if (_use_precalc) ++cache_reads;
        else ++per_bar_computes;
        x = (_use_precalc ? _precalc__ta_ema_1[bar_index_] : (history_advances_new_bar() ? _ta_ema_1.compute(current_bar_.close) : _ta_ema_1.recompute(current_bar_.close)));
        chart_seen.push_back(x);
    }

    void precalculate(const Bar* bars, int n) {
        _use_precalc = false;
        if (n <= 0 || bars == nullptr) return;

        _precalc__ta_ema_1.resize(n);

        _ta_ema_1 = ta::EMA(kLength);

        for (int i = 0; i < n; ++i) {
            _precalc__ta_ema_1[i] = _ta_ema_1.compute(bars[i].close);
        }

        _ta_ema_1 = ta::EMA(kLength);

        _use_precalc = true;
    }
};

struct Observed {
    std::string error;
    int preparations = 0;
    bool allowed = false;
    bool seeding_in_preparation = false;
    int cache_reads = 0;
    int per_bar_computes = 0;
    std::vector<double> chart_seen;
    std::vector<int> chart_flags;
    int partial_evaluations = 0;
    int other_evaluations = 0;
    std::vector<std::int64_t> security_opens;
    std::vector<double> security_closes;
    std::vector<double> security_values;
    std::vector<int> security_flags;
    bool ambient_after = false;
};

using Metadata = std::vector<std::pair<std::string, double>>;

// batch: run(bars, n, "", "") -- no named timeframe, so the engine allows
// precalculation, and the request.security surface is configured. Otherwise
// the same feed with the chart timeframe named, on the per-bar path.
Observed run_script(const Metadata& metadata, const std::vector<Bar>& bars, bool batch) {
    ta::ema_na_warmup_flag() = false;
    SecurityStrategy host;
    for (const auto& item : metadata) host.set_syminfo_metadata(item.first, item.second);
    if (batch) {
        host.run(bars.data(), static_cast<int>(bars.size()), "", "");
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    }
    Observed out;
    out.error = host.last_error();
    out.preparations = host.preparations;
    out.allowed = host.allowed;
    out.seeding_in_preparation = host.seeding_in_preparation;
    out.cache_reads = host.cache_reads;
    out.per_bar_computes = host.per_bar_computes;
    out.chart_seen = host.chart_seen;
    out.chart_flags = host.chart_flags;
    out.partial_evaluations = host.partial_evaluations;
    out.other_evaluations = host.other_evaluations;
    out.security_opens = host.security_opens;
    out.security_closes = host.security_closes;
    out.security_values = host.security_values;
    out.security_flags = host.security_flags;
    out.ambient_after = ta::ema_na_warmup_flag();
    ta::ema_na_warmup_flag() = false;
    return out;
}

// Forty 15-minute bars from a whole hour: ten hours, nonlinear closes.
std::vector<Bar> quarter_feed() {
    std::vector<Bar> bars;
    double previous_close = 50.0;
    for (int i = 0; i < 40; ++i) {
        const int phase = i % 12;
        const double triangle = static_cast<double>(phase < 6 ? phase : 12 - phase);
        const double close = 50.0 + 2.0 * triangle + 0.25 * static_cast<double>((i * 5) % 7)
            + 0.125 * static_cast<double>(i);
        const double open = previous_close;
        bars.push_back(Bar{open, std::max(open, close) + 0.5, std::min(open, close) - 0.5,
                           close, 10.0 + static_cast<double>(i),
                           kStart + static_cast<std::int64_t>(i) * kQuarter});
        previous_close = close;
    }
    return bars;
}

void print_observed(const char* leg, const Observed& o) {
    std::printf("OBS [%s] %s: allowed=%d seeding_in_preparation=%d cache_reads=%d "
                "per_bar_computes=%d chart_bars=%zu chart_first_finite=%d chart_flags_on=%d "
                "hourly_bars=%zu security_first_finite=%d security_flags_on=%d partial=%d "
                "other=%d\n",
                row.c_str(), leg, o.allowed ? 1 : 0, o.seeding_in_preparation ? 1 : 0,
                o.cache_reads, o.per_bar_computes, o.chart_seen.size(),
                first_finite(o.chart_seen), count_of(o.chart_flags, 1),
                o.security_opens.size(), first_finite(o.security_values),
                count_of(o.security_flags, 1), o.partial_evaluations, o.other_evaluations);
    for (std::size_t i = 0; i < o.security_opens.size(); ++i)
        std::printf("OBS [%s] %s: hourly bar %zu open_ms=%lld close=%.17g ema=%.17g flag=%d\n",
                    row.c_str(), leg, i, static_cast<long long>(o.security_opens[i]),
                    o.security_closes[i], o.security_values[i], o.security_flags[i]);
    for (std::size_t i = 0; i < o.chart_seen.size() && i < 6; ++i)
        std::printf("OBS [%s] %s: chart bar %zu ema=%.17g\n", row.c_str(), leg, i,
                    o.chart_seen[i]);
}

struct Policy {
    const char* name;
    Metadata metadata;
    bool chart_sma;
    bool security_sma;
};

// One leg of a policy: the run, the seeding defaults it saw, the hourly bars
// and the security EMA over them.
void check_leg(const char* leg, const Observed& o, const std::vector<Bar>& bars,
               const Policy& policy, bool batch) {
    const std::string name(leg);
    const long long n = static_cast<long long>(bars.size());
    if (!o.error.empty())
        std::printf("FAIL [%s] %s: run error: %s\n", row.c_str(), leg, o.error.c_str());
    expect(o.error.empty(), name + ": the run reports no error");
    expect_int(o.preparations, 1, name + ": preparations");
    expect(o.allowed == batch, name + ": the engine's precalculation flag");
    expect_int(o.cache_reads, batch ? n : 0, name + ": chart values read from the cache");
    expect_int(o.per_bar_computes, batch ? 0 : n, name + ": chart values computed per bar");
    expect_int(static_cast<long long>(o.chart_seen.size()), n, name + ": bars dispatched");
    expect(!o.ambient_after, name + ": the ambient seeding default is restored");
    expect_int(o.other_evaluations, 0, name + ": evaluations of another request");
    // The chart callback runs under the chart policy on every bar.
    expect_int(count_of(o.chart_flags, policy.chart_sma ? 1 : 0), n,
               name + ": chart callbacks under the chart policy");
    // The security evaluation runs under the security policy on every hourly bar.
    const long long hourly = static_cast<long long>(o.security_opens.size());
    expect(hourly >= 6, name + ": at least six completed hourly bars");
    expect_int(count_of(o.security_flags, policy.security_sma ? 1 : 0), hourly,
               name + ": security evaluations under the security policy");
    // The hourly bars are whole hours of the feed, in order, each closing on
    // its last 15-minute bar.
    for (std::size_t i = 0; i < o.security_opens.size(); ++i) {
        const std::int64_t open = o.security_opens[i];
        expect((open - kStart) % kHour == 0, name + ": hourly bar opens on a whole hour");
        if (i > 0)
            expect_int(open - o.security_opens[i - 1], kHour, name + ": hourly bars one hour apart");
        const std::int64_t last_quarter = (open + kHour - kQuarter - kStart) / kQuarter;
        const bool in_feed = open >= kStart && last_quarter >= 0 && last_quarter < n;
        expect(in_feed, name + ": hourly bar inside the feed");
        if (in_feed)
            expect(same(o.security_closes[i], bars[static_cast<std::size_t>(last_quarter)].close),
                   name + ": hourly close is the close of its last 15-minute bar");
    }
    expect_series(o.security_values, oracle_ema(o.security_closes, kLength, policy.security_sma),
                  false, name + ": security EMA against the definition over the hourly closes");
    if (hourly >= kLength)
        expect_int(first_finite(o.security_values), policy.security_sma ? kLength - 1 : 0,
                   name + ": security EMA first finite hourly bar");
}

void expect_same_security(const Observed& a, const Observed& b, const std::string& what) {
    expect(a.security_opens == b.security_opens, what + ": hourly bar opens");
    expect_series(a.security_closes, b.security_closes, true, what + ": hourly closes");
    expect_series(a.security_values, b.security_values, true, what + ": security EMA");
    expect(a.security_flags == b.security_flags, what + ": seeding default in the evaluation");
}

void finish_row(int failures_before) {
    std::printf("%s [%s]\n", failures == failures_before ? "PASS" : "FAIL", row.c_str());
}

}  // namespace

int main() {
    const std::vector<Bar> bars = quarter_feed();
    const double origin = static_cast<double>(bars.front().timestamp);
    const std::vector<Policy> policies{
        {"nothing declared", {}, true, false},
        {"chart_ema_na_warmup=1", {{"chart_ema_na_warmup", 1.0}}, true, false},
        {"chart_ema_na_warmup=0", {{"chart_ema_na_warmup", 0.0}}, false, false},
        {"series_origin_ms, then security_range_start_na_warmup=0",
         {{"series_origin_ms", origin}, {"security_range_start_na_warmup", 0.0}}, true, false},
        {"series_origin_ms", {{"series_origin_ms", origin}}, true, true},
        {"security_range_start_na_warmup", {{"security_range_start_na_warmup", origin}}, true,
         true},
        {"security_range_start_na_warmup, chart_ema_na_warmup=0",
         {{"security_range_start_na_warmup", origin}, {"chart_ema_na_warmup", 0.0}}, false, true},
    };
    std::vector<double> closes;
    for (const Bar& bar : bars) closes.push_back(bar.close);

    Observed undeclared_batch;
    Observed undeclared_per_bar;
    for (std::size_t index = 0; index < policies.size(); ++index) {
        const Policy& policy = policies[index];
        row = policy.name;
        const int before = failures;
        const Observed batch = run_script(policy.metadata, bars, /*batch=*/true);
        const Observed per_bar = run_script(policy.metadata, bars, /*batch=*/false);
        print_observed("batch", batch);
        print_observed("per-bar", per_bar);
        check_leg("batch", batch, bars, policy, /*batch=*/true);
        check_leg("per-bar", per_bar, bars, policy, /*batch=*/false);
        // The chart EMA follows the chart policy on both paths, and only it.
        const std::vector<double> chart = oracle_ema(closes, kLength, policy.chart_sma);
        expect_series(per_bar.chart_seen, chart, false, "per-bar: chart EMA against the definition");
        expect_series(batch.chart_seen, chart, false, "batch: cached chart EMA against the definition");
        expect_series(batch.chart_seen, per_bar.chart_seen, true,
                      "batch: cached chart EMA against the per-bar series");
        expect_int(first_finite(batch.chart_seen), policy.chart_sma ? kLength - 1 : 0,
                   "batch: cached chart EMA first finite bar");
        if (index == 0) {
            undeclared_batch = batch;
            undeclared_per_bar = per_bar;
        } else if (!policy.security_sma) {
            // A chart-only declaration leaves the request.security series as it was.
            expect_same_security(batch, undeclared_batch, "batch against nothing declared");
            expect_same_security(per_bar, undeclared_per_bar, "per-bar against nothing declared");
        }
        finish_row(before);
    }

    std::printf("chart EMA batch security policy: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
