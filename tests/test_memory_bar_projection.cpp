// Bar projection populations are derived from the submitted inputs.
// No byte measurements, product fixtures, direct container writes or early-exit
// check macro. Each dispatch entry runs independently after earlier failures.
#include <pineforge/native_c_api.h>
#include <pineforge/native_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Subscriptions { friend auto access(Subscriptions); };
struct Appended { friend auto access(Appended); };
struct PrepareFeeds { friend auto access(PrepareFeeds); };
template struct Access<Subscriptions, &NativeExecutionConsumer::subscriptions_>;
template struct Access<Appended, &NativeExecutionConsumer::auxiliary_appended_>;
template struct Access<PrepareFeeds, &BacktestEngine::prepare_native_security_feeds>;

std::size_t checks = 0, failures = 0;

bool check(const char* row, bool ok, const char* file, int line,
           const char* expression, const char* explanation) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s -- %s\n",
                     row, file, line, expression, explanation);
    }
    return ok;
}
#define ROW_CHECK(row, expression, explanation) \
    check(row, static_cast<bool>(expression), __FILE__, __LINE__, #expression, explanation)

constexpr std::int64_t minute = 60000;
// 2024-01-01 00:00 UTC, on every minute/hour grid used below.
constexpr std::int64_t start = INT64_C(1704067200000);

std::vector<Bar> make_bars(std::size_t n, int minutes) {
    std::vector<Bar> result;
    for (std::size_t i = 0; i < n; ++i) {
        const double p = 100.0 + static_cast<double>(i % 13) * 0.25;
        result.push_back({p, p + 2.0, p - 2.0, p + 0.5,
                          1.0 + static_cast<double>(i % 7),
                          start + static_cast<std::int64_t>(i) * minutes * minute});
    }
    return result;
}

bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && a.open == b.open && a.high == b.high
        && a.low == b.low && a.close == b.close && a.volume == b.volume;
}

bool same_bars(const std::vector<Bar>& a, const std::vector<Bar>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_bar(a[i], b[i])) return false;
    return true;
}

// Independent OHLCV arithmetic, without the engine's aggregator/calendar code.
std::vector<Bar> reduce(const std::vector<Bar>& input, std::size_t width) {
    std::vector<Bar> output;
    for (std::size_t first = 0; first + width <= input.size(); first += width) {
        Bar bar = input[first];
        bar.volume = 0.0;
        for (std::size_t i = first; i < first + width; ++i) {
            bar.high = std::max(bar.high, input[i].high);
            bar.low = std::min(bar.low, input[i].low);
            bar.close = input[i].close;
            bar.volume += input[i].volume;
        }
        output.push_back(bar);
    }
    return output;
}

NativeRunSpec spec(const char* key, std::uint64_t run, const char* tf = "15") {
    NativeRunSpec value;
    value.identity = {key, run};
    value.input_tf = value.script_tf = tf;
    value.ticker = "BARS";
    value.tickerid = "TEST:BARS";
    value.type = "crypto";
    value.currency = value.basecurrency = "USD";
    value.timezone = "UTC";
    value.session = "24x7";
    value.initial_capital = 10000.0;
    value.point_value = value.account_fx = 1.0;
    value.price_tick = 0.25;
    value.fee_kind = NativeFeeKind::CashPerExecution;
    value.fee_value = 0.0;
    value.report_policy = NativeReportPolicy::KernelRecorded;
    return value;
}

class Host final : public NativeStrategyHost {
public:
    void on_native_run_begin() override { bars_seen = 0; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++bars_seen; }
    const auto& states() const { return security_eval_states_; }
    const auto& feeds() const { return native_security_feeds_; }
    std::size_t bars_seen = 0;
};

void run(const char* row, Host& host, const std::vector<Bar>& input) {
    host.run(input.data(), static_cast<int>(input.size()));
    ROW_CHECK(row, host.last_run_status() == 0 && host.last_error().empty(),
              "the real native run must complete before its retained state is evidence");
    ROW_CHECK(row, host.bars_seen == input.size(), "every input calculated a script bar");
}

// A requested interval has one final bar, one first-input coordinate and one
// completion tag. The model derives all four populations from disjoint input
// groups, rather than consulting the product's cursors or delivery counters.
struct Projection {
    std::vector<Bar> bars;
    std::vector<int> first_index;
    std::vector<std::int64_t> first_ms;
    std::vector<NativeCompletionKind> completion;
};

Projection model_projection(const std::vector<Bar>& feed, std::size_t group,
                            std::size_t feed_bars_per_input) {
    Projection result;
    result.bars = reduce(feed, group);
    for (std::size_t first = 0; first + group <= feed.size(); first += group) {
        result.first_index.push_back(static_cast<int>(first / feed_bars_per_input));
        result.first_ms.push_back(feed[first].timestamp);
        result.completion.push_back(NativeCompletionKind::Confirmed);
    }
    return result;
}

enum class Field { Bars, FirstIndex, FirstMs, Completion };

std::size_t check_projection(const char* row, const Host& host,
                             const Projection& model, Field field) {
    const auto& subscriptions = NativeExecutionConsumer::bound(host).*access(Subscriptions{});
    std::size_t count = 0;
    bool equal = true;
    for (const auto& s : subscriptions) {
        switch (field) {
        case Field::Bars:
            count += s.projected_bars.size();
            equal = same_bars(s.projected_bars, model.bars) && equal;
            break;
        case Field::FirstIndex:
            count += s.projected_first_index.size();
            equal = (s.projected_first_index == model.first_index) && equal;
            break;
        case Field::FirstMs:
            count += s.projected_first_ms.size();
            equal = (s.projected_first_ms == model.first_ms) && equal;
            break;
        case Field::Completion:
            count += s.projected_completion.size();
            equal = (s.projected_completion == model.completion) && equal;
            break;
        }
    }
    // Sum across the real owners as well as checking each sequence: a stale
    // subscription surviving begin must not hide behind the newest instance.
    ROW_CHECK(row, count == model.bars.size() && equal,
              "the named projected container equals the independent interval model, with no old owner");
    return count;
}

void projection_case(const char* row, Field field) {
    Host input_host;
    std::uint64_t generation = 0;
    for (std::size_t n : {std::size_t{32}, std::size_t{128}}) {
        auto config = spec("bars-input", ++generation);
        NativeTimeframeSubscription hourly;
        hourly.tf = "60";
        hourly.lookahead = true;
        config.subscriptions.push_back(hourly);
        ROW_CHECK(row, input_host.configure_native(config).status == NativeSetupStatus::Applied,
                  "configure the input-built lookahead subscription");
        const auto input = make_bars(n, 15);
        run(row, input_host, input);
        // 60/15 = four disjoint inputs per interval: 8 and 32 output rows.
        check_projection(row, input_host, model_projection(input, 4, 1), field);
    }

    // A fixed caller-supplied auxiliary feed is independent of the number of
    // chart bars: 120 minutes / 5 minutes = 24 projected rows at BOTH lengths.
    // This deliberately drives the other projection insert site too.
    Host auxiliary_host;
    const auto feed = make_bars(120, 1);
    const auto auxiliary_model = model_projection(feed, 5, 15);
    std::array<std::size_t, 2> retained{};
    generation = 0;
    for (std::size_t trial = 0; trial < retained.size(); ++trial) {
        auto config = spec("bars-aux-projection", ++generation);
        config.auxiliary_feed = NativeAuxiliaryFeed{"1", feed};
        NativeTimeframeSubscription five;
        five.tf = "5";
        five.source = NativeSeriesSource::AuxiliaryFeed;
        five.lookahead = true;
        config.subscriptions.push_back(five);
        ROW_CHECK(row, auxiliary_host.configure_native(config).status == NativeSetupStatus::Applied,
                  "configure the auxiliary-built lookahead subscription");
        run(row, auxiliary_host, make_bars(trial == 0 ? 16 : 64, 15));
        retained[trial] = check_projection(row, auxiliary_host, auxiliary_model, field);
    }
    ROW_CHECK(row, retained[0] == retained[1] && retained[1] == auxiliary_model.bars.size(),
              "the fixed auxiliary projection has the same exact count after N and 4N chart bars");

    // Removing the only request retires all four real vectors at the next
    // begin. The model is empty because the new specification has no sites.
    auto empty = spec("bars-aux-projection", ++generation);
    ROW_CHECK(row, auxiliary_host.configure_native(empty).status == NativeSetupStatus::Applied,
              "configure a later run without subscriptions");
    run(row, auxiliary_host, make_bars(16, 15));
    check_projection(row, auxiliary_host, Projection{}, field);

    // Realtime continuation does not append live history to the historical
    // lookahead projection: the 32-bar warmup's eight intervals stay eight
    // while the same stream grows to 128 bars.
    Host stream;
    auto config = spec("bars-warmup", 1);
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    hourly.lookahead = true;
    config.subscriptions.push_back(hourly);
    const auto all = make_bars(128, 15);
    const std::vector<Bar> warmup(all.begin(), all.begin() + 32);
    const auto warmup_model = model_projection(warmup, 4, 1);
    ROW_CHECK(row, stream.configure_native(config).status == NativeSetupStatus::Applied,
              "configure the lookahead warmup");
    if (!ROW_CHECK(row, stream.stream_begin(all.data(), 32, "15", "15"),
                   "begin the real stream")) return;
    const auto at_n = check_projection(row, stream, warmup_model, field);
    for (std::size_t i = 32; i < all.size(); ++i)
        ROW_CHECK(row, stream.stream_push_bar(all[i]), "accept a confirmed live input");
    const auto at_4n = check_projection(row, stream, warmup_model, field);
    ROW_CHECK(row, at_n == at_4n && at_4n == warmup_model.bars.size(),
              "the historical projection stays fixed while realtime bars quadruple");
    ROW_CHECK(row, stream.stream_end(false), "finish the real stream");
}

void bars_case(const char* row) { projection_case(row, Field::Bars); }
void index_case(const char* row) { projection_case(row, Field::FirstIndex); }
void time_case(const char* row) { projection_case(row, Field::FirstMs); }
void completion_case(const char* row) { projection_case(row, Field::Completion); }

void appended_case(const char* row) {
    Host host;
    const auto minute_feed = make_bars(90, 1);
    const std::vector<Bar> declared(minute_feed.begin(), minute_feed.begin() + 30);
    const std::vector<Bar> appended(minute_feed.begin() + 30, minute_feed.end());
    std::array<std::size_t, 2> retained{};
    for (std::size_t trial = 0; trial < retained.size(); ++trial) {
        auto config = spec("bars-appended", trial + 1);
        config.auxiliary_feed = NativeAuxiliaryFeed{"1", declared};
        NativeTimeframeSubscription five;
        five.tf = "5";
        five.source = NativeSeriesSource::AuxiliaryFeed;
        config.subscriptions.push_back(five);
        ROW_CHECK(row, host.configure_native(config).status == NativeSetupStatus::Applied,
                  "configure the declared warmup feed");
        const auto input = make_bars(trial == 0 ? 16 : 64, 15);
        if (!ROW_CHECK(row, host.stream_begin(input.data(), 2, "15", "15"),
                       "begin with two chart bars and thirty declared feed bars")) continue;
        const auto& actual = NativeExecutionConsumer::bound(host).*access(Appended{});
        ROW_CHECK(row, actual.size() == 0,
                  "begin retires the preceding run's appended population, including on reuse");
        for (std::size_t i = 2; i < input.size(); ++i) {
            if (i < 6) {
                const auto first = i * 15;
                ROW_CHECK(row, host.append_auxiliary_bars(minute_feed.data() + first, 15),
                          "append this input's fifteen finer bars through the public stream path");
            }
            ROW_CHECK(row, host.stream_push_bar(input[i]), "accept the live chart bar");
        }
        // Four appends of fifteen bars = sixty; later chart bars add no feed
        // values. Inspect the member, not auxiliary_bar_count (which also
        // includes the separate configuration vector).
        ROW_CHECK(row, actual.size() == appended.size() && same_bars(actual, appended),
                  "the entire retained append vector equals exactly the four submitted slices");
        retained[trial] = actual.size();
        ROW_CHECK(row, host.stream_end(false), "finish the auxiliary-feed stream");
    }
    ROW_CHECK(row, retained[0] == retained[1] && retained[1] == appended.size(),
              "a fixed appended feed stays sixty rows at N and 4N chart bars; no universal stream cap is claimed");
}

bool equal_index(const Host& host, const std::vector<Bar>& feed) {
    if (host.states().size() != 1) return false;
    const auto& actual = host.states().front().native_bars_by_label;
    if (actual.size() != feed.size()) return false;
    // The fixture's hourly UTC stamps each name a distinct, aligned period.
    // No production labeler is used to compute the expected map keys.
    for (const auto& bar : feed) {
        const auto found = actual.find(bar.timestamp);
        if (found == actual.end() || !same_bar(found->second, bar)) return false;
    }
    return true;
}

bool equal_feeds(const Host& host, const std::vector<Bar>& feed) {
    return host.feeds().size() == 1 && host.feeds().front().tf == "60"
        && same_bars(host.feeds().front().bars, feed);
}

void feed_case(const char* row, bool index) {
    Host host;
    const auto feed = make_bars(8, 60);
    std::array<std::size_t, 2> retained{};
    for (std::size_t trial = 0; trial < retained.size(); ++trial) {
        auto config = spec("bars-authoritative", trial + 1);
        NativeTimeframeSubscription hourly;
        hourly.tf = "60";
        config.subscriptions.push_back(hourly);
        ROW_CHECK(row, host.configure_native(config).status == NativeSetupStatus::Applied,
                  "configure an aggregating hourly request");
        ROW_CHECK(row, host.set_native_security_feed("60", feed.data(), static_cast<int>(feed.size())),
                  "install caller-owned hourly bars through the real feed setter");
        run(row, host, make_bars(trial == 0 ? 32 : 128, 15));
        if (index) {
            ROW_CHECK(row, equal_index(host, feed),
                      "the complete native label map equals the eight independent hourly key/value pairs");
            if (host.states().size() == 1)
                retained[trial] = host.states().front().native_bars_by_label.size();
        } else {
            ROW_CHECK(row, equal_feeds(host, feed),
                      "the stored feed is one exact copy of the eight supplied bars");
            if (host.feeds().size() == 1) retained[trial] = host.feeds().front().bars.size();
        }
    }
    ROW_CHECK(row, retained[0] == retained[1] && retained[1] == feed.size(),
              "fixed native feed input retains the same count when chart bars quadruple");

    // Change the real ingress while Completed, then invoke the exact production
    // preparation routine on the EXISTING evaluator. A fresh evaluator would
    // conceal a missing map clear. No member is seeded or edited by the test.
    const std::vector<Bar> smaller(feed.begin(), feed.begin() + 3);
    ROW_CHECK(row, host.set_native_security_feed("60", smaller.data(), static_cast<int>(smaller.size())),
              "replace the hourly feed with a shorter caller population");
    if (index) {
        const auto input = make_bars(128, 15);
        (host.*access(PrepareFeeds{}))(input.data(), static_cast<int>(input.size()));
        ROW_CHECK(row, equal_index(host, smaller),
                  "preparation retires five obsolete keys and retains exactly the three replacement keys");
    } else {
        ROW_CHECK(row, equal_feeds(host, smaller),
                  "replacement releases the former feed rows and retains exactly the three supplied rows");
        ROW_CHECK(row, host.set_native_security_feed("60", nullptr, 0), "remove the configured feed");
        ROW_CHECK(row, host.feeds().size() == 0, "zero-count ingress retires the whole native feed owner");
    }
}
void label_case(const char* row) { feed_case(row, true); }
void stored_feed_case(const char* row) { feed_case(row, false); }

struct CHandle {
    pf_strategy_t value = nullptr;
    ~CHandle() { if (value) strategy_native_host_free(value); }
};

struct CStimulus {
    pf_strategy_t handle = nullptr;
    const char* row = nullptr;
    std::size_t bars = 0, closes = 0;
};

int c_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& stimulus = *static_cast<CStimulus*>(user);
    const auto index = stimulus.bars++;
    if (index % 3 == 2) return 0;
    pf_native_request_v1 request{};
    request.struct_size = sizeof request;
    request.version = PF_NATIVE_API_VERSION;
    request.intent = index % 3 == 0 ? PF_NATIVE_INTENT_TRANSACT : PF_NATIVE_INTENT_FLATTEN;
    request.intent_value = index % 3 == 0 ? 1.0 : 0.0;
    request.trigger = PF_NATIVE_TRIGGER_MARKET;
    request.label = "bar-request";
    std::uint64_t incarnation = 0;
    ROW_CHECK(stimulus.row, strategy_native_submit_v1(stimulus.handle, &request, &incarnation, nullptr)
                           == PF_NATIVE_OK,
              "the C-configured run uses the real entry/flatten submit path");
    return 0;
}

int c_applied(void* user, const pf_native_applied_v1* value, const pf_native_decision_v1*) {
    auto& stimulus = *static_cast<CStimulus*>(user);
    if (value->closed_units > 0.0) ++stimulus.closes;
    return 0;
}

bool c_spec_equal(const BacktestEngine& engine, const std::vector<Bar>& authority,
                  const std::vector<Bar>& lower, const std::vector<Bar>& auxiliary) {
    const auto* actual = NativeExecutionConsumer::bound(engine).view().spec;
    if (!actual || actual->subscriptions.size() != 1 || !actual->intrabar.lower()
        || !actual->auxiliary_feed) return false;
    return same_bars(actual->subscriptions.front().authoritative_bars, authority)
        && same_bars(actual->intrabar.lower()->bars, lower)
        && same_bars(actual->auxiliary_feed->bars, auxiliary);
}

void c_host_case(const char* row) {
    CStimulus stimulus;
    stimulus.row = row;
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = sizeof callbacks;
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &stimulus;
    callbacks.on_bar = c_bar;
    callbacks.on_applied = c_applied;
    CHandle handle{strategy_native_host_create_v1(&callbacks)};
    if (!ROW_CHECK(row, handle.value != nullptr, "create a real C callback host")) return;
    stimulus.handle = handle.value;
    // The C handle is the BacktestEngine base pointer by construction
    // through the C host API; no anonymous CCallbackHost type is named.
    const auto& engine = *static_cast<const BacktestEngine*>(handle.value);
    const auto lower = make_bars(640, 1);
    const auto auxiliary = make_bars(640, 1);
    const auto authority = make_bars(12, 60);
    const auto chart = reduce(lower, 5);

    pf_native_run_spec_v1 base{};
    base.struct_size = sizeof base;
    base.session_key = "bars-c-host";
    base.input_tf = base.script_tf = "5";
    base.ticker = "BARS";
    base.tickerid = "TEST:BARS";
    base.type = "crypto";
    base.currency = base.basecurrency = "USD";
    base.description = base.volumetype = base.chart_timezone = "";
    base.timezone = "UTC";
    base.session = "24x7";
    base.initial_capital = 10000.0;
    base.point_value = base.account_fx = 1.0;
    base.price_tick = 0.25;
    base.fee_kind = PF_NATIVE_FEE_PERCENT;
    base.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;

    pf_native_subscription_v1 subscription{};
    subscription.struct_size = sizeof subscription;
    subscription.tf = "60";
    subscription.authoritative_bars = reinterpret_cast<const pf_bar_t*>(authority.data());
    subscription.authoritative_n = static_cast<int>(authority.size());
    const std::uint32_t source = PF_NATIVE_SERIES_SOURCE_INPUT;
    pf_native_run_spec_ext_v1 ext{};
    ext.struct_size = sizeof ext;
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS | PF_NATIVE_SPEC_EXT_INTRABAR
                     | PF_NATIVE_SPEC_EXT_AUXILIARY_FEED;
    ext.subscriptions = &subscription;
    ext.subscriptions_n = 1;
    ext.subscription_sources = &source;
    ext.intrabar_kind = PF_NATIVE_INTRABAR_LOWER_TF;
    ext.intrabar_tf = "1";
    ext.intrabar_bars = reinterpret_cast<const pf_bar_t*>(lower.data());
    ext.intrabar_n = static_cast<int>(lower.size());
    ext.intrabar_samples = 4;
    ext.intrabar_distribution = PF_MAGNIFIER_ENDPOINTS;
    ext.intrabar_volume_weighted_min_samples = 2;
    ext.intrabar_volume_weighted_max_samples = 64;
    ext.intrabar_sample_eligibility = PF_NATIVE_SAMPLE_CONTINUOUS_SEGMENTS;
    ext.auxiliary_tf = "1";
    ext.auxiliary_bars = reinterpret_cast<const pf_bar_t*>(auxiliary.data());
    ext.auxiliary_n = static_cast<int>(auxiliary.size());

    for (std::size_t n : {std::size_t{32}, std::size_t{128}}) {
        ++base.run_number;
        stimulus.bars = stimulus.closes = 0;
        std::uint32_t error = 0, field = 0;
        if (!ROW_CHECK(row, strategy_configure_native_ext_result_v1(
                           handle.value, &base, &ext, &error, &field) == PF_NATIVE_OK,
                       "translate all three owned bar-vector paths through the C ABI")) return;
        // 1 subscription, 12 hourly authority rows, 640 lower bars and 640
        // auxiliary bars are caller populations. They are identical at N and
        // 4N chart bars and must replace the previous completed run's spec.
        if (!ROW_CHECK(row, c_spec_equal(engine, authority, lower, auxiliary),
                       "C-host configuration owns exactly the caller's three vectors and one subscription")) return;
        ROW_CHECK(row, strategy_native_run_v1(handle.value,
                       reinterpret_cast<const pf_bar_t*>(chart.data()), static_cast<int>(n), nullptr)
                       == PF_NATIVE_OK, "consume the C-configured vectors through the real run path");
        ROW_CHECK(row, stimulus.bars == n && stimulus.closes == n / 3,
                  "the C callbacks drove every bar and the independently scheduled entry/close cycles");
        ROW_CHECK(row, c_spec_equal(engine, authority, lower, auxiliary),
                  "N and 4N input bars leave each configured C-host vector exactly unchanged");
    }
}

struct Case { const char* row; void (*run)(const char*); };
} // namespace

int main() {
    const Case cases[] = {
        {"timeframesubscription_projected_bars", bars_case},
        {"timeframesubscription_projected_first_index", index_case},
        {"timeframesubscription_projected_first_ms", time_case},
        {"timeframesubscription_projected_completion", completion_case},
        {"nativeexecutionconsumer_auxiliary_appended", appended_case},
        {"securityevalstate_native_bars_by_label", label_case},
        {"nativesecurityfeed_bars", stored_feed_case},
        {"c_host_nativerunspec_bar_vectors", c_host_case},
    };
    for (const auto& item : cases) {
        try {
            item.run(item.row);
        } catch (const std::exception& error) {
            ROW_CHECK(item.row, false, error.what());
        } catch (...) {
            ROW_CHECK(item.row, false, "unknown exception in this row; continuing dispatch");
        }
    }
    std::printf("memory proof bar_projection: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
