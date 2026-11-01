// Account logs, daily feed labels and subscriptions follow the configured inputs.
// Counts and reference contents below come from the fixture's schedule/config,
// not recorded output. No product definition or access level is changed.
#include <pineforge/native_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Consumer = NativeExecutionConsumer;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct AccountLog { friend auto access(AccountLog); };
struct DayLabels { friend auto access(DayLabels); };
struct FeedTimeframes { friend auto access(FeedTimeframes); };
template struct Access<AccountLog, &Consumer::account_log_>;
template struct Access<DayLabels, &Consumer::day_labels_>;
template struct Access<FeedTimeframes, &Consumer::subscription_feed_tfs_>;

int checks = 0;
int failures = 0;

#define MEM_CHECK(row, expression)                                             \
    do {                                                                      \
        ++checks;                                                             \
        if (!(expression)) {                                                  \
            ++failures;                                                       \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", row, __FILE__,       \
                         __LINE__, #expression);                              \
        }                                                                     \
    } while (false)

constexpr const char* kAccount = "nativeexecutionconsumer_account_log";
constexpr const char* kLabels = "nativeexecutionconsumer_day_labels";
constexpr const char* kFeeds = "nativeexecutionconsumer_subscription_feed_tfs";
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;
constexpr std::int64_t kMonday = 1704067200000LL; // 2024-01-01 00:00 UTC
constexpr std::int64_t kSunday = kMonday - kDay;

NativeRunSpec configuration(const char* key, const char* input, const char* script) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = input;
    spec.script_tf = script;
    spec.ticker = "RD";
    spec.tickerid = "TEST:RD";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "account-log storage proof";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.chart_timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = .01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    return spec;
}

Bar flat_bar(std::int64_t timestamp) {
    return Bar{100, 101, 99, 100, 1, timestamp};
}

std::vector<Bar> tape(int count, std::int64_t step) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back(flat_bar(kMonday + i * step));
    return bars;
}

class Host final : public NativeStrategyHost {
public:
    explicit Host(const char* row_id) : row(row_id) {}
    const char* row;
    std::uint64_t next_run = 1;
    int cycles = 0;
    int inputs = 0;
    int calculations = 0;
    int submitted = 0;
    std::vector<std::uint64_t> applied_ordinals;
    std::function<void()> inspect;

    const auto& accounts() { return consumer().*access(AccountLog{}); }
    const auto& labels() { return consumer().*access(DayLabels{}); }
    const auto& feed_timeframes() { return consumer().*access(FeedTimeframes{}); }

    void on_native_run_begin() override {
        inputs = calculations = submitted = 0;
        applied_ordinals.clear();
    }
    void on_native_input(const Bar&, const NativeInputContext&) override {
        ++inputs;
        if (inspect) inspect();
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++calculations;
        if (submitted >= 2 * cycles) return;
        // Each pair submits a one-unit entry and a whole-position close.
        // Default NextEligiblePoint fills each on the next input's open.
        const auto result = submitted % 2 == 0
            ? submit_market(no::Request{order_action::Transact{1.0}, "entry", ""})
            : submit_market(no::Request{execution::Flatten{}, "close", ""});
        MEM_CHECK(row, result.status == no::SubmitStatus::Accepted);
        MEM_CHECK(row, result.handle.has_value());
        ++submitted;
    }
    void on_native_applied(const no::ExecutionAppliedEvent& applied,
                           const NativeDecisionContext&) override {
        applied_ordinals.push_back(applied.ordinal);
        if (inspect) inspect();
    }

private:
    Consumer& consumer() { return as_native_consumer(execution_consumer()); }
};

void run(Host& host, NativeRunSpec spec, const std::vector<Bar>& bars) {
    // Public begins require increasing run generations on a reused host.
    spec.identity.run_number = host.next_run++;
    const auto setup = host.configure_native(spec);
    MEM_CHECK(host.row, setup.status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), spec.input_tf, spec.script_tf,
             false, 4, MagnifierDistribution::ENDPOINTS);
    MEM_CHECK(host.row, host.last_error().empty());
    if (!host.last_error().empty())
        std::fprintf(stderr, "[%s] engine: %s\n", host.row, host.last_error().c_str());
    MEM_CHECK(host.row, host.native_state().kind == NativeLifecycleKind::Completed);
    MEM_CHECK(host.row, host.inputs == static_cast<int>(bars.size()));
}

// A constant-price, zero-fee round trip realizes zero cash change. Entry fill
// i=0,2,... leaves +1 unit; close fill i=1,3,... leaves zero. There are exactly
// 2*cycles fills, on tape slots 1..2*cycles, independently of extra idle bars.
void check_account_model(Host& host, const std::vector<Bar>& bars, bool window) {
    const auto fills = static_cast<std::size_t>(2 * host.cycles);
    MEM_CHECK(kAccount, host.submitted == 2 * host.cycles);
    MEM_CHECK(kAccount, host.applied_ordinals.size() == fills);
    MEM_CHECK(kAccount, host.physical_position().signed_units == 0);
    const auto& log = host.accounts();
    MEM_CHECK(kAccount, log.size() == (window ? 0u : fills));
    if (window) {
        MEM_CHECK(kAccount, log.capacity() == 0);
    } else {
        // Size mismatches are already failures; still check every available
        // model row, without indexing past either vector after a mismatch.
        const auto common = std::min(fills, std::min(log.size(), host.applied_ordinals.size()));
        for (std::size_t i = 0; i < common; ++i) {
            MEM_CHECK(kAccount, log[i].ordinal == host.applied_ordinals[i]);
            MEM_CHECK(kAccount, log[i].effective_time_ms == bars[i + 1].timestamp);
            MEM_CHECK(kAccount, log[i].signed_units == (i % 2 == 0 ? 1.0 : 0.0));
            MEM_CHECK(kAccount, log[i].marked_equity == 10000);
            MEM_CHECK(kAccount, log[i].realized_balance == 10000);
        }
    }
}

void account_retention() {
    const auto short_tape = tape(64, kMinute);
    const auto long_tape = tape(4 * 64, kMinute);
    // Full/Commands intentionally have no fill-independent cap. Prove the
    // default Window's zero storage AND expose the owning-log exception.
    for (const auto retention : {NativeEventRetention::Full, NativeEventRetention::Commands}) {
        Host host(kAccount);
        auto spec = configuration("account-log", "1", "1");
        spec.event_retention = retention;
        host.cycles = 6;
        bool window = false;
        host.inspect = [&] {
            const auto expected = window ? 0u : host.applied_ordinals.size();
            MEM_CHECK(kAccount, host.accounts().size() == expected);
            if (window) MEM_CHECK(kAccount, host.accounts().capacity() == 0);
        };
        run(host, spec, short_tape);
        check_account_model(host, short_tape, false);
        const auto at_n = host.accounts().size();
        run(host, spec, long_tape);
        check_account_model(host, long_tape, false);
        MEM_CHECK(kAccount, host.accounts().size() == at_n); // fixed fills, 4N bars

        // Increase the actual driver too: 4*6 round trips -> 2*(4*6) rows.
        // This explicitly demonstrates why Full/Commands are not bounded by
        // live orders alone; neither is claimed to satisfy the Window bound.
        host.cycles *= 4;
        run(host, spec, long_tape);
        check_account_model(host, long_tape, false);
        MEM_CHECK(kAccount, host.accounts().size() == 4 * at_n);

        // Switch this SAME populated host to Window. Both the old rows and
        // their allocation must leave before the next run's first input.
        spec.event_retention = NativeEventRetention::Window;
        window = true;
        host.cycles = 6;
        run(host, spec, short_tape);
        check_account_model(host, short_tape, true);
        const auto window_at_n = host.accounts().size();
        run(host, spec, long_tape);
        check_account_model(host, long_tape, true);
        MEM_CHECK(kAccount, host.accounts().size() == window_at_n);
    }
    // Also prove the untouched default configuration on a fresh consumer.
    Host fresh(kAccount);
    fresh.cycles = 6;
    auto default_spec = configuration("account-default", "1", "1");
    MEM_CHECK(kAccount, default_spec.event_retention == NativeEventRetention::Window);
    run(fresh, default_spec, short_tape);
    check_account_model(fresh, short_tape, true);
    run(fresh, default_spec, long_tape);
    check_account_model(fresh, long_tape, true);
}

// Five sessions/week, opening Sun..Thu at 18:00 UTC, each lasting 23 hours.
// Thus weeks=1 -> 5*23=115 input bars; weeks=4 -> 4*115=460 bars.
std::vector<Bar> session_tape(int weeks) {
    std::vector<Bar> bars;
    for (int week = 0; week < weeks; ++week)
        for (int day = 0; day < 5; ++day)
            for (int hour = 0; hour < 23; ++hour)
                bars.push_back(flat_bar(kSunday + (7 * week + day) * kDay
                                        + (18 + hour) * kHour));
    return bars;
}

void daily_feed_labels() {
    Host host(kLabels);
    auto spec = configuration("daily-labels", "60", "1D");
    spec.session = "1800-1700";
    const auto short_tape = session_tape(1);
    const auto long_tape = session_tape(4);
    using Labels = std::vector<std::pair<std::int64_t, std::int64_t>>;
    Labels expected;
    std::vector<Bar> daily;
    // Three distinct session openings each have a feed stamp one hour before
    // opening (in the 17:00..18:00 break). No input lies inside that gap.
    for (int day = 0; day < 3; ++day) {
        const auto open = kSunday + day * kDay + 18 * kHour;
        daily.push_back(flat_bar(open - kHour));
        expected.emplace_back(open, open - kHour);
        // A second, later stamp for Monday's SAME interval adds no label.
        if (day == 1) daily.push_back(flat_bar(open - kHour / 2));
    }
    // A stamp at Wednesday's actual opening needs no replacement label.
    daily.push_back(flat_bar(kSunday + 3 * kDay + 18 * kHour));
    MEM_CHECK(kLabels, host.set_native_security_feed("D", daily.data(),
                                                    static_cast<int>(daily.size())));
    host.inspect = [&] {
        MEM_CHECK(kLabels, host.labels().size() == expected.size());
        MEM_CHECK(kLabels, host.labels() == expected);
    };
    run(host, spec, short_tape);
    MEM_CHECK(kLabels, host.calculations == 5);
    MEM_CHECK(kLabels, host.labels() == expected);
    const auto at_n = host.labels().size();
    run(host, spec, long_tape);
    MEM_CHECK(kLabels, host.calculations == 4 * 5);
    MEM_CHECK(kLabels, host.labels() == expected);
    MEM_CHECK(kLabels, host.labels().size() == at_n); // same feed, 4N input bars

    // Replace five feed bars with one: the previous labels cannot accumulate.
    daily = {flat_bar(kSunday + 2 * kDay + 17 * kHour)};
    expected = {{kSunday + 2 * kDay + 18 * kHour, daily.front().timestamp}};
    MEM_CHECK(kLabels, host.set_native_security_feed("D", daily.data(), 1));
    run(host, spec, short_tape);
    MEM_CHECK(kLabels, host.labels() == expected);
    run(host, spec, long_tape);
    MEM_CHECK(kLabels, host.labels().size() == expected.size());

    // With no installed daily feed the complete container is empty.
    MEM_CHECK(kLabels, host.set_native_security_feed("D", nullptr, 0));
    expected.clear();
    run(host, spec, short_tape);
    MEM_CHECK(kLabels, host.labels().empty());
    run(host, spec, long_tape);
    MEM_CHECK(kLabels, host.labels() == expected);
}

NativeTimeframeSubscription authoritative(const char* tf, int minutes) {
    NativeTimeframeSubscription subscription;
    subscription.tf = tf;
    // Keep this installed feed FIXED when the script's input grows.
    subscription.authoritative_bars = {flat_bar(kMonday),
                                       flat_bar(kMonday + minutes * kMinute)};
    return subscription;
}

std::vector<std::string> remembered_feed_model(const NativeRunSpec& spec) {
    // Set membership, independent of the product's linear-vector search;
    // preserve first-declaration order for whole-container equality.
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (const auto& subscription : spec.subscriptions)
        if (!subscription.authoritative_bars.empty() && seen.insert(subscription.tf).second)
            result.push_back(subscription.tf);
    return result;
}

void authoritative_feed_names() {
    Host host(kFeeds);
    auto spec = configuration("subscriptions", "15", "15");
    const auto hourly = authoritative("60", 60);
    spec.subscriptions = {hourly, hourly, authoritative("120", 120)};
    NativeTimeframeSubscription aggregated;
    aggregated.tf = "240"; // no authoritative bars: no remembered feed name
    spec.subscriptions.push_back(aggregated);
    auto expected = remembered_feed_model(spec);
    const auto short_tape = tape(16, 15 * kMinute);
    const auto long_tape = tape(4 * 16, 15 * kMinute);
    host.inspect = [&] {
        MEM_CHECK(kFeeds, host.feed_timeframes().size() == expected.size());
        MEM_CHECK(kFeeds, host.feed_timeframes() == expected);
    };
    run(host, spec, short_tape);
    MEM_CHECK(kFeeds, host.calculations == 16);
    MEM_CHECK(kFeeds, host.feed_timeframes() == expected);
    const auto at_n = host.feed_timeframes().size();
    run(host, spec, long_tape);
    MEM_CHECK(kFeeds, host.calculations == 4 * 16);
    MEM_CHECK(kFeeds, host.feed_timeframes() == expected);
    MEM_CHECK(kFeeds, host.feed_timeframes().size() == at_n); // fixed config, 4N bars

    // Change the actual declaration set on the same host. Old names must go,
    // even though the newly declared period was previously aggregation-only.
    spec.subscriptions = {authoritative("240", 240)};
    expected = remembered_feed_model(spec);
    run(host, spec, short_tape);
    MEM_CHECK(kFeeds, host.feed_timeframes() == expected);
    run(host, spec, long_tape);
    MEM_CHECK(kFeeds, host.feed_timeframes().size() == expected.size());

    spec.subscriptions.clear();
    expected = remembered_feed_model(spec);
    run(host, spec, short_tape);
    MEM_CHECK(kFeeds, host.feed_timeframes().empty());
    run(host, spec, long_tape);
    MEM_CHECK(kFeeds, host.feed_timeframes() == expected);
}

struct Row {
    const char* id;
    void (*proof)();
};
} // namespace

int main() {
    const Row rows[] = {
        {kAccount, account_retention},
        {kLabels, daily_feed_labels},
        {kFeeds, authoritative_feed_names},
    };
    for (const auto& row : rows) {
        try {
            row.proof();
        } catch (const std::exception& error) {
            MEM_CHECK(row.id, false);
            std::fprintf(stderr, "[%s] exception: %s\n", row.id, error.what());
        } catch (...) {
            MEM_CHECK(row.id, false);
            std::fprintf(stderr, "[%s] unknown exception\n", row.id);
        }
    }
    std::printf("memory proof account_log_labels: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
