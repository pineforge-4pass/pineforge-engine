#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
using namespace pineforge;

// All three periods open on January 1, 2024 UTC. Only the payload tells
// which feed supplied a bar; the chart cannot supply any of these prices.
constexpr auto stamp = 1704067200000LL;
constexpr Bar chart{10, 14, 8, 12, 1, stamp};
constexpr Bar month{100, 110, 90, 101, 2, stamp};
constexpr Bar quarter{200, 220, 180, 202, 3, stamp};
constexpr Bar year{300, 330, 270, 303, 4, stamp};
constexpr Bar replacement{400, 440, 360, 404, 5, stamp};
constexpr auto minute = 60000LL;
constexpr auto session_open = stamp + 9 * 60 * minute;
constexpr Bar late{500, 550, 450, 505, 6, session_open + 30 * minute};

bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && a.open == b.open && a.high == b.high
        && a.low == b.low && a.close == b.close && a.volume == b.volume;
}

class Host final : public NativeStrategyHost {
public:
    std::vector<std::int64_t> labels;
    void on_native_bar(const Bar&, const NativeDecisionContext& ctx) override {
        labels.push_back(ctx.script_bar_open_ms);
    }

    bool contains(const Bar& bar) const {
        return std::any_of(native_security_feeds_.begin(), native_security_feeds_.end(),
            [&](const auto& feed) {
                return feed.bars.size() == 1 && same_bar(feed.bars.front(), bar);
            });
    }

    bool stores(std::initializer_list<Bar> bars) const {
        return bars.size() == native_security_feeds_.size()
            && std::all_of(bars.begin(), bars.end(), [&](const Bar& b) { return contains(b); });
    }

    bool install(const char* tf, const Bar& bar) {
        return set_native_security_feed(tf, &bar, 1) && contains(bar);
    }

    SecurityEvalState& register_one(const char* tf, const char* input) {
        security_eval_states_.clear();
        register_security_eval(0, tf, input);
        if (security_eval_states_.size() != 1)
            throw std::runtime_error("expected one registered evaluator");
        return security_eval_states_.front();
    }

    bool reads(const char* tf, const Bar* expected, const char* input = "D") {
        auto& state = register_one(tf, input);
        if (!state.aggregator.is_active())
            throw std::runtime_error("expected an active evaluator");
        prepare_native_security_feeds(&chart, 1);
        Bar got = chart;
        const bool selected = substitute_native_security_bar(state, got);
        return selected == (expected != nullptr) && same_bar(got, expected ? *expected : chart);
    }

    bool fractional_clock() {
        auto& state = register_one("1440", "1000");
        if (state.aggregator.is_active())
            throw std::runtime_error("fractional evaluator already active before preparation");
        prepare_native_security_feeds(&chart, 1);
        return state.aggregator.is_active();
    }

    std::int64_t first_hour_open() {
        auto& state = register_one("60", "15");
        if (!state.aggregator.is_active()) throw std::runtime_error("missing hourly evaluator");
        prepare_native_security_feeds(&late, 1);
        return state.aggregator.bucket_open_ms(late.timestamp);
    }

    std::vector<std::int64_t> chart_day_stamps() {
        input_tf_ = "15";
        prepare_chart_day_partition(&late, 1);
        return chart_day_partition_.stamps;
    }

    void split_daily_chart() {
        script_tf_ = "1D";
        script_tf_seconds_ = internal::tf_to_clock_seconds(script_tf_);
        security_input_tf_ = "15";
    }
    bool source_aux_security_feed_enabled() const override { return !security_input_tf_.empty(); }
    void source_aux_security_input_view(const Bar*& bars, int& n) const override {
        bars = &month;
        n = 1;
    }
};

int failures = 0;

template<class Read>
void check(const char* row, bool ready, Read read) {
    if (ready) {
        try {
            const bool ok = read();
            std::printf("%s %s\n", ok ? "PASS" : "FAIL", row);
            if (ok) return;
        } catch (const std::exception& error) {
            std::printf("NOT COMPARED %s: %s\n", row, error.what());
        }
    } else {
        std::printf("NOT COMPARED %s: required feed setup did not complete\n", row);
    }
    ++failures;
}

template<class Prepare>
bool setup(const char* row, Prepare prepare) {
    try {
        if (prepare()) return true;
        std::printf("NOT COMPARED %s: feed setup was refused or its bar was missing\n", row);
    } catch (const std::exception& error) {
        std::printf("NOT COMPARED %s: %s\n", row, error.what());
    }
    return false;
}

void check_installed_periods() {
    std::optional<Host> host;
    const bool seeded = setup("seed M", [&] {
        host.emplace();
        return host->install("M", month) && host->reads("M", &month);
    });
    check("3M lookup with only M installed keeps the chart bar", seeded,
          [&] { return host->reads("3M", nullptr); });

    const bool installed = seeded && setup("install 3M and 12M", [&] {
        return host->install("3M", quarter) && host->install("12M", year);
    });
    check("registry retains M, 3M and 12M", installed,
          [&] { return host->stores({month, quarter, year}); });
    check("M lookup with all feeds", installed, [&] { return host->reads("M", &month); });
    check("3M lookup with all feeds", installed, [&] { return host->reads("3M", &quarter); });
    check("12M lookup with all feeds", installed, [&] { return host->reads("12M", &year); });
}

void check_cleared_period() {
    std::optional<Host> host;
    const bool cleared = setup("seed 3M then M and clear 3M", [&] {
        host.emplace();
        return host->install("3M", quarter) && host->install("M", month)
            && host->reads("M", &month) && host->set_native_security_feed("3M", nullptr, 0);
    });
    check("clearing 3M removes only its bar", cleared, [&] { return host->stores({month}); });
    check("M lookup after clearing 3M", cleared, [&] { return host->reads("M", &month); });
    check("cleared 3M lookup keeps the chart bar", cleared && host->contains(month),
          [&] { return host->reads("3M", nullptr); });
}

void check_one_month_alias() {
    std::optional<Host> host;
    const bool replaced = setup("replace M through 1M", [&] {
        host.emplace();
        return host->install("M", month) && host->install("1M", replacement);
    });
    check("1M replaces the M slot", replaced, [&] { return host->stores({replacement}); });
    check("M lookup after replacing through 1M", replaced,
          [&] { return host->reads("M", &replacement); });
    check("1M lookup after replacing through 1M", replaced,
          [&] { return host->reads("1M", &replacement); });
    check("clearing M removes the 1M replacement", replaced, [&] {
        return host->set_native_security_feed("M", nullptr, 0) && !host->native_security_feed_enabled();
    });
}

void check_declared_feeds() {
    using Series = NativeTimeframeSubscription;
    const auto series_with = [](const char* tf, const Bar& bar) {
        Series series;
        series.tf = tf;
        series.authoritative_bars.push_back(bar);
        return series;
    };
    const auto m = series_with("M", month), q = series_with("3M", quarter);
    const auto y = series_with("12M", year), alias = series_with("1M", replacement);
    const auto validate = [](const std::vector<Series>& series) {
        return validate_native_timeframe_subscriptions(series, "D", false);
    };
    const bool valid = validate({m}).ok() && validate({q}).ok() && validate({y}).ok();
    check("distinct monthly feeds may be declared together", valid, [&] {
        const auto result = validate({m, q, y});
        if (!result.ok() && (result.error != NativeRunSpecError::DuplicateSubscriptionTimeframe
                             || result.field != NativeRunSpecField::SubscriptionBars))
            throw std::runtime_error("unexpected subscription refusal");
        return result.ok();
    });
    const bool aliases_valid = validate({m}).ok() && validate({alias}).ok();
    check("conflicting M and 1M declarations are refused", aliases_valid, [&] {
        const auto result = validate({m, alias});
        return result.error == NativeRunSpecError::DuplicateSubscriptionTimeframe
            && result.field == NativeRunSpecField::SubscriptionBars;
    });
}

void check_unrelated_feeds() {
    Host monthly, daily, fixed, week;
    const bool m = setup("read installed M", [&] {
        return monthly.install("M", month) && monthly.reads("M", &month);
    });
    check("badM cannot select the monthly feed", m, [&] { return monthly.reads("badM", nullptr); });

    const bool d = setup("read weekly fallback from 1D", [&] {
        return daily.install("1D", quarter) && daily.reads("W", &quarter);
    });
    check("badM cannot aggregate the daily feed", d, [&] { return daily.reads("badM", nullptr); });

    const bool f = setup("read fixed 1440 through its seconds alias", [&] {
        return fixed.install("1440", year) && fixed.reads("86400S", &year, "60");
    });
    check("a week cannot aggregate fixed 1440 bars", d && f, [&] { return fixed.reads("W", nullptr); });
    check("only a fixed feed activates a fractional fixed clock", d && f, [&] {
        if (!fixed.fractional_clock()) throw std::runtime_error("fixed feed did not activate its clock");
        return !daily.fractional_clock();
    });

    const bool w = setup("read installed W", [&] {
        return week.install("W", month) && week.reads("W", &month);
    });
    check("7D cannot select a weekly feed", w, [&] { return week.reads("7D", nullptr); });

    Host split;
    split.split_daily_chart();
    const bool implicit = setup("read implicit chart day over finer input", [&] {
        return split.reads("D", &chart) && split.stores({}) && split.install("1440", quarter);
    });
    check("fixed 1440 leaves the implicit chart day in place", implicit, [&] {
        return split.reads("D", &chart) && split.stores({quarter});
    });
}

void check_daily_clocks() {
    Host daily, fixed;
    const bool installed = setup("seed identical off-open stamps on separate hosts", [&] {
        for (Host* host : {&daily, &fixed}) host->set_syminfo_session("0900-1200");
        return daily.install("1D", late) && fixed.install("1440", late);
    });
    check("fixed 1440 cannot move the hourly grid", installed, [&] {
        if (daily.first_hour_open() != late.timestamp)
            throw std::runtime_error("the daily control did not move the hourly open");
        return fixed.first_hour_open() == session_open;
    });
    check("fixed 1440 cannot supply the chart day partition", installed, [&] {
        if (daily.chart_day_stamps() != std::vector<std::int64_t>{late.timestamp})
            throw std::runtime_error("the daily control did not supply its stamp");
        return fixed.chart_day_stamps().empty();
    });
}

std::int64_t delivered_day_label(const char* feed_tf) {
    Host host;
    NativeRunSpec spec;
    spec.identity = {"day-label", 1};
    spec.tickerid = "TEST:DAY";
    spec.input_tf = "15";
    spec.script_tf = "D";
    spec.timezone = "UTC";
    spec.session = "0900-1200";
    spec.initial_capital = 1000;
    spec.point_value = spec.account_fx = 1;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied || !host.install(feed_tf, late))
        throw std::runtime_error("could not configure the day-label run or install its feed");
    // No input before 09:30; the last 15-minute input reaches the noon close.
    std::vector<Bar> inputs;
    for (auto t = late.timestamp; t < session_open + 3 * 60 * minute; t += 15 * minute) {
        inputs.push_back(chart);
        inputs.back().timestamp = t;
    }
    host.run(inputs.data(), static_cast<int>(inputs.size()), spec.input_tf, spec.script_tf);
    if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
    if (host.labels.size() != 1) throw std::runtime_error("expected one daily callback");
    return host.labels.front();
}

void check_fixed_supplier() {
    NativeTimeframeSubscription request, supplier;
    request.tf = "1440";
    supplier.tf = "86400S";
    supplier.authoritative_bars = {quarter};
    const auto validate_pair = [&] {
        return validate_native_timeframe_subscriptions({request, supplier}, "1000", false);
    };
    const bool supplied = validate_pair().ok();
    supplier.tf = "D";
    check("a daily declaration cannot supply a fractional fixed request", supplied, [&] {
        if (!validate_native_timeframe_subscriptions({supplier}, "1000", false).ok())
            throw std::runtime_error("daily supplier was not valid on its own");
        const auto result = validate_pair();
        return result.error == NativeRunSpecError::InvalidSubscriptionTimeframe
            && result.field == NativeRunSpecField::SubscriptionTimeframe;
    });
}
}  // namespace

int main() {
    check_installed_periods();
    check_cleared_period();
    check_one_month_alias();
    check_declared_feeds();
    check_unrelated_feeds();
    check_daily_clocks();
    const bool dated = setup("read native day label from 1D", [] {
        return delivered_day_label("1D") == late.timestamp;
    });
    check("fixed 1440 cannot date the native script day", dated, [] {
        return delivered_day_label("1440") == session_open;
    });
    check_fixed_supplier();
    return failures == 0 ? 0 : 1;
}
